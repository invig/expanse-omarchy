#define WLR_USE_UNSTABLE

#include "Render.hpp"
#include "Canvas.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <cctype>
#include <format>
#include <cmath>
#include <unordered_map>

// Hyprland's function hooks only work on x86_64, so the canvas draws from
// render-stage events. At the start of a frame the canvas windows are hidden
// from Hyprland's own pass; after its windows, each one is drawn where the
// camera puts it by giving it that box as its current (animated) geometry for
// the rest of the frame, the way Hyprland draws a window mid-animation.
// Hyprland clips a surface to its untransformed box, so render modifiers
// can't move a window that really sits off-screen.
namespace Expanse::Render {

    // Explicit instantiation may name members it can't access, which hands us
    // three that Hyprland keeps protected: the renderer's per-window draw, a
    // window's raw hidden flag (setHidden() also drops focus), and an animated
    // variable's "being animated" flag.
    template <typename Tag, typename Tag::type Member>
    struct SRob {
        friend typename Tag::type robbed(Tag) {
            return Member;
        }
    };

    struct SRenderWindowTag {
        using type = void (::Render::IHyprRenderer::*)(PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, ::Render::eRenderPassMode, bool, bool);
        friend type robbed(SRenderWindowTag);
    };

    struct SHiddenTag {
        using type = bool Desktop::View::CWindow::*;
        friend type robbed(SHiddenTag);
    };

    struct SBeingAnimatedTag {
        using type = bool Hyprutils::Animation::CBaseAnimatedVariable::*;
        friend type robbed(SBeingAnimatedTag);
    };

    template struct SRob<SRenderWindowTag, &::Render::IHyprRenderer::renderWindow>;
    template struct SRob<SHiddenTag, &Desktop::View::CWindow::m_hidden>;
    template struct SRob<SBeingAnimatedTag, &Hyprutils::Animation::CBaseAnimatedVariable::m_bIsBeingAnimated>;

    // Without decorations: their boxes are laid out for the window's real size.
    static void renderWindow(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now) {
        (g_pHyprRenderer.get()->*robbed(SRenderWindowTag{}))(window, monitor, now, false, ::Render::RENDER_PASS_ALL, false, false);
    }

    static void setHiddenQuietly(const PHLWINDOW& window, bool hidden) {
        window.get()->*robbed(SHiddenTag{}) = hidden;
    }

    // As on the Mac: the accent for highlights, pink snap guides, a dark ×.
    static const CHyprColor ACCENT       = {0.54F, 0.71F, 0.98F, 1.F};
    static const CHyprColor GUIDE        = {1.F, 0.18F, 0.33F, 1.F};
    static const CHyprColor CLOSE_FILL   = {0.1F, 0.1F, 0.1F, 0.75F};
    static const CHyprColor LABEL        = {1.F, 1.F, 1.F, 0.95F};
    static const CHyprColor LABEL_SHADOW = {0.F, 0.F, 0.F, 0.7F};
    constexpr float         DIM_ALPHA    = 0.45F;
    constexpr double        LABEL_PT     = 11.0;
    constexpr double        LABEL_GAP    = 5.0;
    constexpr double        LABEL_MIN    = 70.0; // narrower on screen, a window shows no label

    struct SRestore {
        PHLWINDOWREF window;
        Vector2D     position;
        Vector2D     size;
        bool         animatingIn   = false;
        bool         sizeAnimating = false;
    };

    struct SFrame {
        PHLMONITORREF          monitor;
        std::vector<PHLWINDOW> hidden;
        std::vector<SRestore>  restore;
        Time::steady_tp        now;
    };
    static SFrame g_frame;

    // Text is drawn once and kept until it changes.
    struct SText {
        std::string               text;
        int                       width = 0;
        int                       px    = 0;
        SP<::Render::ITexture>    tex;
        SP<::Render::ITexture>    shadow;
    };
    static std::unordered_map<const Desktop::View::CWindow*, SText> g_labels;
    static SText                                                     g_closeGlyph;

    static CBox toPixels(const CBox& screen, const PHLMONITOR& monitor) {
        return screen.copy().translate(-monitor->m_position).scale(monitor->m_scale).round();
    }

    static void add(UP<IPassElement>&& element) {
        g_pHyprRenderer->addPassElement(std::move(element));
    }

    static void addRect(const CBox& px, const CHyprColor& color, int round = 0) {
        CRectPassElement::SRectData rect;
        rect.box   = px;
        rect.color = color;
        rect.round = round;
        add(makeUnique<CRectPassElement>(rect));
    }

    static void addBorder(const CBox& px, const CHyprColor& color, int width, int round) {
        CBorderPassElement::SBorderData border;
        border.box        = px;
        border.grad1      = Config::CGradientValueData{color};
        border.borderSize = width;
        border.round      = round;
        border.outerRound = round + width;
        add(makeUnique<CBorderPassElement>(border));
    }

    static void addTexture(const SP<::Render::ITexture>& tex, const Vector2D& px) {
        if (!tex)
            return;
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = {px, tex->m_size};
        add(makeUnique<CTexPassElement>(std::move(data)));
    }

    static bool refresh(SText& cached, const std::string& text, int px, int width, const CHyprColor& color, bool shadow) {
        if (cached.tex && cached.text == text && cached.px == px && cached.width == width)
            return true;
        cached = {text, width, px, g_pHyprRenderer->renderText(text, color, px, false, "", width, 500), nullptr};
        if (shadow)
            cached.shadow = g_pHyprRenderer->renderText(text, LABEL_SHADOW, px, false, "", width, 500);
        return cached.tex != nullptr;
    }

    // "org.gnome.Nautilus" → "Nautilus", "foot" → "Foot".
    static std::string appName(const PHLWINDOW& window) {
        std::string name = window->m_class;
        if (const auto DOT = name.rfind('.'); DOT != std::string::npos && DOT + 1 < name.size())
            name = name.substr(DOT + 1);
        if (!name.empty())
            name[0] = std::toupper(sc<unsigned char>(name[0]));
        return name.empty() ? window->m_title : name;
    }

    static void addLabel(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& screen, bool hovered) {
        if (screen.w < LABEL_MIN)
            return;
        const std::string NAME = appName(window);
        const std::string TEXT = hovered && !window->m_title.empty() && window->m_title != NAME ? NAME + " — " + window->m_title : NAME;
        // Text is redrawn when its width changes, so while zooming it changes in 32 px steps.
        const int WIDTH = std::max(sc<int>(std::floor(std::max((screen.w - 4) * monitor->m_scale, 60.0) / 32.0)) * 32, 60);
        const int PX    = std::round(LABEL_PT * monitor->m_scale);

        auto& cached = g_labels[window.get()];
        if (!refresh(cached, TEXT, PX, WIDTH, LABEL, true))
            return;
        const CBox     PIXELS = toPixels(screen, monitor);
        const Vector2D AT     = {PIXELS.x + 2 * monitor->m_scale, PIXELS.y + PIXELS.h + LABEL_GAP * monitor->m_scale};
        addTexture(cached.shadow, AT + Vector2D{0.0, sc<double>(monitor->m_scale)});
        addTexture(cached.tex, AT);
    }

    static void addCloseButton(const PHLMONITOR& monitor, const CBox& screen) {
        const CBox PIXELS = toPixels(CCanvas::closeButton(screen), monitor);
        addRect(PIXELS, CLOSE_FILL, PIXELS.w / 2);
        if (!refresh(g_closeGlyph, "✕", std::round(PIXELS.h * 0.5), 0, CHyprColor{1.F, 1.F, 1.F, 1.F}, false))
            return;
        addTexture(g_closeGlyph.tex, (PIXELS.middle() - g_closeGlyph.tex->m_size / 2.0).round());
    }

    static void addGuide(const PHLMONITOR& monitor, const Arrange::SGuide& guide) {
        const Vector2D A = (g_canvas.toScreen(guide.from) - monitor->m_position) * monitor->m_scale;
        const Vector2D B = (g_canvas.toScreen(guide.to) - monitor->m_position) * monitor->m_scale;
        const double   W = std::max(1.0, std::round(sc<double>(monitor->m_scale)));
        addRect(CBox{std::min(A.x, B.x) - (A.x == B.x ? W / 2 : 0), std::min(A.y, B.y) - (A.y == B.y ? W / 2 : 0), std::max(std::abs(B.x - A.x), W),
                     std::max(std::abs(B.y - A.y), W)}
                    .round(),
                GUIDE);
    }

    static SText g_zoomText;

    // As on the Mac: a dark panel, windows as light blocks, the view outlined,
    // the zoom underneath.
    static void addMinimap(const PHLMONITOR& monitor, const SMinimap& map, double scale) {
        const double S    = monitor->m_scale;
        const int    LINE = std::max(1, sc<int>(std::round(S)));
        const CBox   BOX  = toPixels(map.box, monitor);
        addRect(BOX, CHyprColor{0.12F, 0.12F, 0.12F, 0.88F}, std::round(12 * S));
        addBorder(BOX.copy().expand(-LINE), CHyprColor{1.F, 1.F, 1.F, 0.08F}, LINE, std::round(12 * S) - LINE);

        for (const auto& window : map.windows)
            addRect(toPixels(map.toScreen(window), monitor), CHyprColor{0.85F, 0.85F, 0.85F, 0.75F}, std::round(1.5 * S));

        const CBox VIEW = toPixels(map.toScreen(map.viewport), monitor);
        addRect(VIEW, CHyprColor{1.F, 1.F, 1.F, 0.06F}, std::round(2 * S));
        addBorder(VIEW.copy().expand(-LINE), CHyprColor{1.F, 1.F, 1.F, 0.9F}, LINE, std::round(2 * S));

        if (refresh(g_zoomText, std::format("{}%", sc<int>(std::round(scale * 100))), std::round(10 * S), 0, CHyprColor{1.F, 1.F, 1.F, 0.7F}, false))
            addTexture(g_zoomText.tex, Vector2D{BOX.x + 10 * S, BOX.y + BOX.h - 7 * S - g_zoomText.tex->m_size.y}.round());
    }

    static SText              g_spacesHeading;
    static std::vector<SText> g_spaceNames;

    // As the Spaces bar on the Mac: a panel like the minimap's, a tile per
    // workspace (here a small map of it: the screen, its windows), the name
    // underneath. The one you're on is outlined in the accent; the one under
    // the pointer lights up.
    static void addSpacesBar(const PHLMONITOR& monitor, const SSpacesBar& bar) {
        const double S    = monitor->m_scale;
        const int    LINE = std::max(1, sc<int>(std::round(S)));
        const CBox   BOX  = toPixels(bar.box, monitor);
        addRect(BOX, CHyprColor{0.12F, 0.12F, 0.12F, 0.88F}, std::round(12 * S));
        addBorder(BOX.copy().expand(-LINE), CHyprColor{1.F, 1.F, 1.F, 0.08F}, LINE, std::round(12 * S) - LINE);

        if (refresh(g_spacesHeading, "Workspaces", std::round(10 * S), 0, CHyprColor{1.F, 1.F, 1.F, 0.55F}, false))
            addTexture(g_spacesHeading.tex, Vector2D{BOX.x + 12 * S, BOX.y + 9 * S}.round());

        g_spaceNames.resize(bar.tiles.size());
        for (size_t i = 0; i < bar.tiles.size(); ++i) {
            const auto& tile  = bar.tiles[i];
            const CBox  TILE  = toPixels(tile.box, monitor);
            const float SHOWN = tile.current || tile.hovered ? 1.F : 0.85F;
            const int   ROUND = std::round(6 * S);
            addRect(TILE, CHyprColor{0.05F, 0.05F, 0.05F, SHOWN}, ROUND);
            addRect(toPixels(tile.screen, monitor), CHyprColor{0.3F, 0.3F, 0.3F, 0.9F * SHOWN}, std::round(1.5 * S));
            for (const auto& window : tile.windows)
                addRect(toPixels(window, monitor), CHyprColor{0.85F, 0.85F, 0.85F, 0.8F * SHOWN}, std::round(S));

            const int WIDTH = tile.current ? 2 * LINE : LINE;
            addBorder(TILE.copy().expand(-WIDTH), tile.current ? ACCENT : CHyprColor{1.F, 1.F, 1.F, tile.hovered ? 0.7F : 0.15F}, WIDTH, ROUND - WIDTH);

            if (refresh(g_spaceNames[i], tile.name, std::round(10 * S), std::round(TILE.w + 4 * S), CHyprColor{1.F, 1.F, 1.F, 0.8F}, false))
                addTexture(g_spaceNames[i].tex, Vector2D{TILE.x + (TILE.w - g_spaceNames[i].tex->m_size.x) / 2, TILE.y + TILE.h + 7 * S}.round());
        }
    }

    static void hideCanvasWindows(PHLMONITOR monitor) {
        if (!g_canvas.drawsOn(monitor) || monitor->m_activeWorkspace != g_canvas.workspace())
            return;

        g_frame.monitor = monitor;
        g_frame.hidden  = g_canvas.canvasWindows();
        g_frame.now     = Time::steadyNow();
        for (const auto& window : g_frame.hidden)
            setHiddenQuietly(window, true);
    }

    static void unhideCanvasWindows() {
        for (const auto& window : g_frame.hidden)
            setHiddenQuietly(window, false);
        g_frame.hidden.clear();
    }

    static void restoreGeometry() {
        for (const auto& saved : g_frame.restore) {
            const auto WINDOW = saved.window.lock();
            if (!WINDOW)
                continue;
            WINDOW->positionAnimation()->value()                    = saved.position;
            WINDOW->sizeAnimation()->value()                        = saved.size;
            WINDOW->m_animatingIn                                   = saved.animatingIn;
            (*WINDOW->sizeAnimation()).*robbed(SBeingAnimatedTag{}) = saved.sizeAnimating;
        }
        g_frame.restore.clear();
    }

    static void drawCanvas() {
        const auto MONITOR = g_frame.monitor.lock();
        if (g_frame.hidden.empty() || !MONITOR || g_pHyprRenderer->m_renderData.pMonitor != MONITOR)
            return;

        const auto WINDOWS = g_frame.hidden;
        unhideCanvasWindows();

        // Everything placed through the camera first: it's read from the
        // geometry about to change.
        const auto        HOVER = g_canvas.hovered();
        std::vector<CBox> screens;
        std::vector<bool> closes;
        for (const auto& window : WINDOWS) {
            screens.push_back(g_canvas.screenBox(window));
            closes.push_back(g_canvas.showsClose(window));
        }
        const auto SELECTIONBOUNDS = g_canvas.selectionBounds();
        const auto GUIDES          = g_canvas.guides();
        const auto MARQUEE         = g_canvas.marquee();
        const auto MINIMAP         = g_canvas.minimap();
        const auto SPACESBAR       = g_canvas.spacesBar();
        const bool DECORATED       = g_canvas.showsCanvas();

        const float DIM = DIM_ALPHA * sc<float>(g_canvas.zoomedOut());
        if (DIM > 0.F)
            addRect({0, 0, MONITOR->m_transformedSize.x, MONITOR->m_transformedSize.y}, CHyprColor{0.F, 0.F, 0.F, DIM});

        for (size_t i = 0; i < WINDOWS.size(); ++i) {
            const auto& window = WINDOWS[i];
            auto&       size   = window->sizeAnimation();

            g_frame.restore.push_back({window, window->positionAnimation()->value(), size->value(), window->m_animatingIn, size->isBeingAnimated()});

            window->positionAnimation()->value() = screens[i].pos() - window->m_floatingOffset;
            size->value()                        = screens[i].size();
            // Scale the surfaces into that box instead of cropping them to it.
            window->m_animatingIn                = true;
            (*size).*robbed(SBeingAnimatedTag{}) = true;

            renderWindow(window, MONITOR, g_frame.now);

            if (!DECORATED)
                continue;

            // Details stay the same size on screen at any zoom.
            const bool SELECTED = g_canvas.isSelected(window);
            if (SELECTED || window == HOVER) {
                const int WIDTH = std::round((SELECTED ? 3.0 : 2.0) * MONITOR->m_scale);
                addBorder(toPixels(screens[i], MONITOR), ACCENT, WIDTH, std::round(window->rounding() * g_canvas.scale() * MONITOR->m_scale));
            }
            if (closes[i])
                addCloseButton(MONITOR, screens[i]);
            addLabel(MONITOR, window, screens[i], window == HOVER);
        }

        if (!DECORATED)
            return;

        if (SELECTIONBOUNDS) {
            const CBox SCREEN{g_canvas.toScreen(SELECTIONBOUNDS->pos()), SELECTIONBOUNDS->size() * g_canvas.scale()};
            addBorder(toPixels(SCREEN.copy().expand(8), MONITOR), ACCENT.modifyA(0.7F), std::max(1, sc<int>(std::round(MONITOR->m_scale))), std::round(6 * MONITOR->m_scale));
        }

        for (const auto& guide : GUIDES)
            addGuide(MONITOR, guide);

        if (MARQUEE) {
            const CBox PIXELS = toPixels(*MARQUEE, MONITOR);
            addRect(PIXELS, ACCENT.modifyA(0.12F));
            addBorder(PIXELS, ACCENT, std::max(1, sc<int>(std::round(MONITOR->m_scale))), 0);
        }

        if (MINIMAP)
            addMinimap(MONITOR, *MINIMAP, g_canvas.scale());
        if (SPACESBAR)
            addSpacesBar(MONITOR, *SPACESBAR);
    }

    void onStage(eRenderStage stage) {
        switch (stage) {
            case RENDER_BEGIN:
                if (const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock())
                    hideCanvasWindows(MONITOR);
                break;
            // Below the bar and other top layers, above the wallpaper.
            case RENDER_POST_WINDOWS: drawCanvas(); break;
            // A frame that skipped the workspace (no damage) still gets its
            // windows back.
            case RENDER_LAST_MOMENT: unhideCanvasWindows(); break;
            // The pass has drawn; the windows get their real geometry back.
            case RENDER_POST:
                unhideCanvasWindows();
                restoreGeometry();
                if (!g_canvas.live())
                    g_labels.clear();
                break;
            default: break;
        }
    }

    void shutdown() {
        unhideCanvasWindows();
        restoreGeometry();
        g_labels.clear();
        g_closeGlyph = {};
        g_zoomText   = {};
        g_spacesHeading = {};
        g_spaceNames.clear();
    }
}
