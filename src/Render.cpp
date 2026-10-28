#define WLR_USE_UNSTABLE

#include "Render.hpp"
#include "Canvas.hpp"
#include "Mode.hpp"
#include "Perf.hpp"
#include "Toast.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <cairo/cairo.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <format>
#include <cmath>
#include <functional>
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

    struct SInputBlockTag {
        using type = uint32_t Desktop::View::CWindow::*;
        friend type robbed(SInputBlockTag);
    };

    struct SFadeoutsTag {
        using type = std::vector<SP<Desktop::IFadeout>> Desktop::CFadingOutState::*;
        friend type robbed(SFadeoutsTag);
    };

    template struct SRob<SRenderWindowTag, &::Render::IHyprRenderer::renderWindow>;
    template struct SRob<SInputBlockTag, &Desktop::View::CWindow::m_inputBlockReasons>;
    template struct SRob<SHiddenTag, &Desktop::View::CWindow::m_hidden>;
    template struct SRob<SBeingAnimatedTag, &Hyprutils::Animation::CBaseAnimatedVariable::m_bIsBeingAnimated>;
    template struct SRob<SFadeoutsTag, &Desktop::CFadingOutState::m_fadeouts>;

    // Without decorations (their boxes are laid out for the window's real
    // size) or popups (theirs aren't moved with it). As on the Mac, where a
    // menu is a window of its own and closes as the canvas takes the
    // keyboard, an open menu isn't on the canvas; back at 100% it's there.
    static void renderWindow(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now) {
        (g_pHyprRenderer.get()->*robbed(SRenderWindowTag{}))(window, monitor, now, false, ::Render::RENDER_PASS_MAIN, false, false);
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
    static const CHyprColor LABEL_QUIET  = {0.75F, 0.75F, 0.75F, 0.9F};
    static const CHyprColor LABEL_LIGHT  = {0.32F, 0.32F, 0.32F, 1.F};
    // The canvas without the desktop picture, in the theme's light or dark.
    static const CHyprColor PLAIN_DARK   = {0.13F, 0.13F, 0.13F, 1.F};
    static const CHyprColor PLAIN_LIGHT  = {0.85F, 0.85F, 0.85F, 1.F};
    // Expanse's shadow is black at 22%, 14 pt of blur, 6 pt down. Hyprland's
    // shader fades out over its whole range, so it reaches twice the blur
    // radius and starts darker, to look the same.
    constexpr float         SHADOW_ALPHA  = 0.4F;
    constexpr double        SHADOW_RADIUS = 28.0;
    constexpr double        SHADOW_OFFSET = 6.0;
    constexpr double        LABEL_PT     = 11.0;
    constexpr double        LABEL_GAP    = 5.0;
    constexpr double        LABEL_MIN    = 70.0; // narrower on screen, a window shows no label
    // A texture made while a frame is on the GPU waits for it to finish (in
    // the VM, up to tens of ms), so text is made ahead while nothing moves.
    // While the camera moves a label keeps the text it has (cropped to its
    // window), and only a few missing ones are made each frame.
    constexpr int           LABELS_PER_FRAME = 4;

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
        std::vector<PHLWINDOW> foreign; // other monitors' windows, not drawn on this one
        std::vector<SRestore>  restore;
        Time::steady_tp        now;
    };
    static SFrame g_frame;
    static PHLMONITORREF g_rendering; // the monitor this frame is for: Hyprland forgets it before RENDER_POST

    // Text is drawn once and kept until it changes.
    struct SText {
        std::string               text;
        int                       width = 0; // what it was made to fit; 0, any
        int                       px    = 0;
        CHyprColor                color;
        SP<::Render::ITexture>    tex;
        SP<::Render::ITexture>    shadow;
        bool                      whole = false; // not shortened: it fits anywhere at least as wide
    };
    static std::unordered_map<const Desktop::View::CWindow*, SText> g_labels;
    static SText                                                     g_closeGlyph;
    static int                                                       g_labelsMade = 0; // this frame

    static CBox toPixels(const CBox& screen, const PHLMONITOR& monitor) {
        return screen.copy().translate(-monitor->m_position).scale(monitor->m_scale).round();
    }

    static void add(UP<IPassElement>&& element) {
        g_pHyprRenderer->addPassElement(std::move(element));
    }

    // Hyprland aborts on a rect, border or texture with no width or height
    // (a marquee dragged straight sideways, a window mid-map), so nothing
    // empty is ever handed to it.
    static bool drawable(const CBox& px) {
        return px.w >= 1 && px.h >= 1;
    }

    static void addRect(const CBox& px, const CHyprColor& color, int round = 0) {
        if (!drawable(px))
            return;
        CRectPassElement::SRectData rect;
        rect.box   = px;
        rect.color = color;
        rect.round = round;
        add(makeUnique<CRectPassElement>(rect));
    }

    // Hyprland draws a border outside its box, `points` wide.
    static void addBorder(const CBox& px, const CHyprColor& color, int points, int round) {
        if (!drawable(px))
            return;
        CBorderPassElement::SBorderData border;
        border.box        = px;
        border.grad1      = Config::CGradientValueData{color};
        border.borderSize = points;
        border.round      = std::max(round, 0);
        add(makeUnique<CBorderPassElement>(border));
    }

    // A line just inside the box, as a Core Animation layer's border.
    static void addOutline(const PHLMONITOR& monitor, const CBox& px, const CHyprColor& color, int points, int round) {
        const double INSET = points * monitor->m_scale;
        addBorder(px.copy().expand(-INSET), color, points, round - INSET);
    }

    // Expanse's window shadow, through Hyprland's shadow shader. A pass
    // element of our own: Hyprland's needs a window decoration.
    constexpr const char* SHADOW_PASS = "CExpanseShadowElement";

    class CShadowElement : public IPassElement {
      public:
        CShadowElement(const CBox& px, int round, float roundingPower, int range, float alpha) :
            m_box(px), m_round(round), m_roundingPower(roundingPower), m_range(range), m_alpha(alpha) {}

        std::vector<UP<IPassElement>> draw() override {
            g_pHyprRenderer->disableScissor();
            g_pHyprRenderer->blend(true);
            g_pHyprRenderer->drawShadow(m_box, m_round, m_roundingPower, m_range, Config::CGradientValueData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, m_alpha);
            return {};
        }
        bool needsLiveBlur() override {
            return false;
        }
        bool needsPrecomputeBlur() override {
            return false;
        }
        const char* passName() override {
            return SHADOW_PASS;
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        CBox  m_box;
        int   m_round;
        float m_roundingPower;
        int   m_range;
        float m_alpha;
    };

    // As on the Mac, in canvas units so it shrinks with the window. It fades
    // in as you zoom out: Omarchy's windows have no shadow at 100%.
    static void addShadow(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& screen, double scale, float shown) {
        if (shown <= 0.F || !drawable(toPixels(screen, monitor)))
            return;
        const double S     = monitor->m_scale;
        const double RANGE = std::max(1.0, SHADOW_RADIUS * scale);
        const CBox   BOX   = screen.copy().expand(RANGE).translate({0.0, SHADOW_OFFSET * scale});
        add(makeUnique<CShadowElement>(toPixels(BOX, monitor), std::round((window->rounding() * scale + RANGE) * S), window->roundingPower(), std::round(RANGE * S),
                                       SHADOW_ALPHA * shown));
    }

    // The window's own border, in its theme's colours, where the camera puts
    // it: Hyprland lays its decorations out for the window's real box, so the
    // windows are drawn without them.
    static void addWindowBorder(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& screen, double scale) {
        const int  SIZE = window->getRealBorderSize();
        const CBox BOX  = toPixels(screen, monitor);
        if (SIZE <= 0 || !drawable(BOX))
            return;
        CBorderPassElement::SBorderData border;
        border.box           = BOX;
        border.grad1         = window->m_realBorderColor;
        border.borderSize    = SIZE;
        border.round         = std::round(window->rounding() * scale * monitor->m_scale);
        border.roundingPower = window->roundingPower();
        border.a             = window->effectiveAlpha();
        border.window        = window;
        if (window->m_borderFadeAnimationProgress->isBeingAnimated()) {
            border.hasGrad2 = true;
            border.grad1    = window->m_realBorderColorPrevious;
            border.grad2    = window->m_realBorderColor;
            border.lerp     = window->m_borderFadeAnimationProgress->value();
        }
        add(makeUnique<CBorderPassElement>(border));
    }

    static void addTexture(const SP<::Render::ITexture>& tex, const Vector2D& px, const CBox& clip = {}) {
        if (!tex || !drawable({px, tex->m_size}))
            return;
        CTexPassElement::SRenderData data;
        data.tex     = tex;
        data.box     = {px, tex->m_size};
        data.clipBox = clip;
        add(makeUnique<CTexPassElement>(std::move(data)));
    }

    static bool refresh(SText& cached, const std::string& text, int px, int width, const CHyprColor& color, bool shadow) {
        if (cached.tex && cached.text == text && cached.px == px && cached.width == width && cached.color == color)
            return true;
        Perf::CTextTime timed;
        cached = {text, width, px, color, g_pHyprRenderer->renderText(text, color, px, false, "", width, 500), nullptr};
        // Pango ends a shortened line within a character or two of its width.
        cached.whole = cached.tex && (width <= 0 || cached.tex->m_size.x < width - 2 * px);
        if (shadow)
            cached.shadow = g_pHyprRenderer->renderText(text, LABEL_SHADOW, px, false, "", width, 500);
        return cached.tex != nullptr;
    }

    static CHyprColor labelColor(bool plain) {
        // White with a shadow over the desktop picture; quiet grey over the plain background.
        return !plain ? LABEL : g_canvas.lightTheme() ? LABEL_LIGHT : LABEL_QUIET;
    }

    static void addLabel(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& screen, bool hovered, bool plain) {
        if (screen.w < LABEL_MIN)
            return;
        const std::string NAME = appName(window);
        const std::string TEXT = hovered && !window->m_title.empty() && window->m_title != NAME ? NAME + " — " + window->m_title : NAME;
        // Text is made again when its width changes, so while zooming it changes in 32 px steps.
        const int WIDTH = std::max(sc<int>(std::floor(std::max((screen.w - 4) * monitor->m_scale, 60.0) / 32.0)) * 32, 60);
        const int PX    = std::round(LABEL_PT * monitor->m_scale);

        auto&      cached  = g_labels[window.get()];
        const auto COLOR   = labelColor(plain);
        const bool SAME    = cached.tex && cached.text == TEXT && cached.px == PX && cached.color == COLOR;
        const bool CURRENT = SAME && (cached.whole ? cached.tex->m_size.x <= WIDTH : cached.width == WIDTH);
        const bool KEEP    = cached.tex && cached.px == PX && g_canvas.cameraMoving();
        if (!CURRENT && !KEEP) {
            if (g_labelsMade >= LABELS_PER_FRAME)
                g_canvas.wantFrame(); // the rest next frame
            else {
                ++g_labelsMade;
                refresh(cached, TEXT, PX, WIDTH, COLOR, !plain);
            }
        }
        if (!cached.tex)
            return;

        // As on the Mac, just above the window, cropped to its width.
        const CBox     PIXELS = toPixels(screen, monitor);
        const double   S      = monitor->m_scale;
        const Vector2D AT     = Vector2D{PIXELS.x + 2 * S, PIXELS.y - LABEL_GAP * S - cached.tex->m_size.y}.round();
        const CBox     CLIP   = {AT.x, AT.y - S, std::max(PIXELS.w - 4 * S, 1.0), cached.tex->m_size.y + 3 * S};
        if (cached.shadow)
            addTexture(cached.shadow, AT + Vector2D{0.0, S}, CLIP);
        addTexture(cached.tex, AT, CLIP);
    }

    // Labels of windows that have gone.
    static void pruneLabels() {
        std::erase_if(g_labels, [](const auto& entry) {
            return std::ranges::none_of(Desktop::windowState()->windows(), [&](const PHLWINDOW& window) { return window.get() == entry.first; });
        });
    }

    static void addCloseButton(const PHLMONITOR& monitor, const CBox& screen) {
        const CBox PIXELS = toPixels(CCanvas::closeButton(screen), monitor);
        addRect(PIXELS, CLOSE_FILL, PIXELS.w / 2);
        if (!refresh(g_closeGlyph, "✕", std::round(PIXELS.h * 0.5), 0, CHyprColor{1.F, 1.F, 1.F, 1.F}, false))
            return;
        addTexture(g_closeGlyph.tex, (PIXELS.middle() - g_closeGlyph.tex->m_size / 2.0).round());
    }

    // The selection's outline, dashed as on the Mac (Core Animation's 5 and 4
    // points, 1 pt wide, 6 pt corners). A draw per dash would be hundreds a
    // frame, so a long strip of dashes each way and a rounded square for the
    // corners are made once per scale and cropped to each edge and corner.
    struct SDashes {
        double                 scale = 0;
        SP<::Render::ITexture> across, down, corners;
    };
    static SDashes g_dashes;
    constexpr int  DASH_STRIP = 4096; // px; a longer edge takes more than one

    static SP<::Render::ITexture> cairoTexture(int w, int h, const std::function<void(cairo_t*)>& draw) {
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        auto* cairo   = cairo_create(surface);
        cairo_set_source_rgba(cairo, ACCENT.r, ACCENT.g, ACCENT.b, ACCENT.a);
        draw(cairo);
        cairo_surface_flush(surface);
        auto tex = g_pHyprRenderer->createTexture(surface);
        cairo_destroy(cairo);
        cairo_surface_destroy(surface);
        return tex;
    }

    static const SDashes& dashes(double scale) {
        if (g_dashes.scale == scale && g_dashes.across && g_dashes.down && g_dashes.corners)
            return g_dashes;
        const int    T   = std::max(1, sc<int>(std::round(scale)));
        const int    R   = std::max(T, sc<int>(std::round(6 * scale)));
        const double ON  = 5 * scale;
        const double OFF = 4 * scale;
        g_dashes.across  = cairoTexture(DASH_STRIP, T, [&](cairo_t* cairo) {
            for (double x = 0; x < DASH_STRIP; x += ON + OFF)
                cairo_rectangle(cairo, x, 0, ON, T);
            cairo_fill(cairo);
        });
        g_dashes.down    = cairoTexture(T, DASH_STRIP, [&](cairo_t* cairo) {
            for (double y = 0; y < DASH_STRIP; y += ON + OFF)
                cairo_rectangle(cairo, 0, y, T, ON);
            cairo_fill(cairo);
        });
        // A circle's outline, 2R across, just inside the texture: its quarters
        // are the corners.
        g_dashes.corners = cairoTexture(2 * R, 2 * R, [&](cairo_t* cairo) {
            cairo_set_line_width(cairo, T);
            cairo_arc(cairo, R, R, R - T / 2.0, 0, 2 * M_PI);
            cairo_stroke(cairo);
        });
        g_dashes.scale   = scale;
        return g_dashes;
    }

    static void addDashedOutline(const CBox& px, double scale) {
        const auto& DASHES = dashes(scale);
        if (!DASHES.across || !DASHES.down || !DASHES.corners)
            return;
        const double T = DASHES.across->m_size.y;
        const double R = DASHES.corners->m_size.x / 2.0;
        if (px.w < 2 * R || px.h < 2 * R)
            return;
        const auto CROPPED = [](const SP<::Render::ITexture>& tex, const Vector2D& at, const CBox& clip) {
            if (!drawable(clip))
                return;
            CTexPassElement::SRenderData data;
            data.tex     = tex;
            data.box     = {at, tex->m_size};
            data.clipBox = clip;
            add(makeUnique<CTexPassElement>(std::move(data)));
        };
        // The edges between the corners.
        for (double x = px.x + R; x < px.x + px.w - R; x += DASH_STRIP) {
            const double LENGTH = std::min<double>(DASH_STRIP, px.x + px.w - R - x);
            CROPPED(DASHES.across, {x, px.y}, {x, px.y, LENGTH, T});
            CROPPED(DASHES.across, {x, px.y + px.h - T}, {x, px.y + px.h - T, LENGTH, T});
        }
        for (double y = px.y + R; y < px.y + px.h - R; y += DASH_STRIP) {
            const double LENGTH = std::min<double>(DASH_STRIP, px.y + px.h - R - y);
            CROPPED(DASHES.down, {px.x, y}, {px.x, y, T, LENGTH});
            CROPPED(DASHES.down, {px.x + px.w - T, y}, {px.x + px.w - T, y, T, LENGTH});
        }
        // The corners, a quarter of the circle each.
        CROPPED(DASHES.corners, {px.x, px.y}, {px.x, px.y, R, R});
        CROPPED(DASHES.corners, {px.x + px.w - 2 * R, px.y}, {px.x + px.w - R, px.y, R, R});
        CROPPED(DASHES.corners, {px.x, px.y + px.h - 2 * R}, {px.x, px.y + px.h - R, R, R});
        CROPPED(DASHES.corners, {px.x + px.w - 2 * R, px.y + px.h - 2 * R}, {px.x + px.w - R, px.y + px.h - R, R, R});
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

    // The zoom under the minimap, set from digits made once per size: it
    // changes every frame of a zoom.
    using SDigits = std::array<SP<::Render::ITexture>, 11>; // 0-9, then %
    static std::unordered_map<int, SDigits> g_digits;

    static const SDigits& digits(int px) {
        auto& set = g_digits[px];
        if (!set[0])
            for (size_t i = 0; i < set.size(); ++i) {
                Perf::CTextTime timed;
                set[i] = g_pHyprRenderer->renderText(i < 10 ? std::to_string(i) : "%", CHyprColor{1.F, 1.F, 1.F, 0.7F}, px, false, "", 0, 500);
            }
        return set;
    }

    // Bottom left at `at`.
    static void addZoomText(int percent, int px, const Vector2D& at) {
        const auto& SET = digits(px);
        if (std::ranges::any_of(SET, [](const auto& tex) { return !tex; }))
            return;
        Vector2D pos = at;
        for (const char C : std::format("{}%", percent)) {
            const auto& TEX = SET[C == '%' ? 10 : C - '0'];
            addTexture(TEX, Vector2D{pos.x, pos.y - TEX->m_size.y}.round());
            pos.x += TEX->m_size.x;
        }
    }

    // As on the Mac: a dark panel, windows as light blocks, the view outlined,
    // the zoom underneath.
    static void addMinimap(const PHLMONITOR& monitor, const SMinimap& map, double scale) {
        const double S   = monitor->m_scale;
        const CBox   BOX = toPixels(map.box, monitor);
        addRect(BOX, CHyprColor{0.12F, 0.12F, 0.12F, 0.88F}, std::round(12 * S));
        addOutline(monitor, BOX, CHyprColor{1.F, 1.F, 1.F, 0.08F}, 1, std::round(12 * S));

        for (const auto& window : map.windows)
            addRect(toPixels(map.toScreen(window), monitor), CHyprColor{0.85F, 0.85F, 0.85F, 0.75F}, std::round(1.5 * S));

        const CBox VIEW = toPixels(map.toScreen(map.viewport), monitor);
        addRect(VIEW, CHyprColor{1.F, 1.F, 1.F, 0.06F}, std::round(2 * S));
        addOutline(monitor, VIEW, CHyprColor{1.F, 1.F, 1.F, 0.9F}, 1, std::round(2 * S));

        addZoomText(sc<int>(std::round(scale * 100)), std::round(10 * S), {BOX.x + 10 * S, BOX.y + BOX.h - 7 * S});
    }

    static SText              g_spacesHeading;
    static std::vector<SText> g_spaceNames;

    // As the Spaces bar on the Mac: a panel like the minimap's, a tile per
    // workspace (here a small map of it: the screen, its windows), the name
    // underneath. The one you're on is outlined in the accent; the one under
    // the pointer lights up.
    // What the monitor's desktop picture (its background layer) shows now.
    static SP<::Render::ITexture> desktopPicture(const PHLMONITOR& monitor) {
        for (const auto& ref : monitor->m_layerSurfaceLayers[0]) {
            const auto LAYER   = ref.lock();
            const auto SURFACE = LAYER && LAYER->m_mapped && LAYER->wlSurface() ? LAYER->wlSurface()->resource() : nullptr;
            if (SURFACE && SURFACE->m_current.texture)
                return SURFACE->m_current.texture;
        }
        return nullptr;
    }

    static void addSpacesBar(const PHLMONITOR& monitor, const SSpacesBar& bar, bool plain) {
        const double S   = monitor->m_scale;
        // As on the Mac, each tile shows the desktop picture (unless the canvas is plain).
        const auto PICTURE = plain ? nullptr : desktopPicture(monitor);
        const CBox   BOX = toPixels(bar.box, monitor);
        addRect(BOX, CHyprColor{0.12F, 0.12F, 0.12F, 0.88F}, std::round(12 * S));
        addOutline(monitor, BOX, CHyprColor{1.F, 1.F, 1.F, 0.08F}, 1, std::round(12 * S));

        if (refresh(g_spacesHeading, "Workspaces", std::round(10 * S), 0, CHyprColor{1.F, 1.F, 1.F, 0.55F}, false))
            addTexture(g_spacesHeading.tex, Vector2D{BOX.x + 12 * S, BOX.y + 9 * S}.round());

        g_spaceNames.resize(bar.tiles.size());
        for (size_t i = 0; i < bar.tiles.size(); ++i) {
            const auto& tile  = bar.tiles[i];
            const CBox  TILE  = toPixels(tile.box, monitor);
            const float SHOWN = tile.current || tile.hovered ? 1.F : 0.85F;
            const int   ROUND = std::round(6 * S);
            addRect(TILE, CHyprColor{0.05F, 0.05F, 0.05F, SHOWN}, ROUND);
            const CBox SCREEN = toPixels(tile.screen, monitor);
            if (PICTURE && drawable(SCREEN)) {
                CTexPassElement::SRenderData picture;
                picture.tex   = PICTURE;
                picture.box   = SCREEN;
                picture.a     = SHOWN;
                picture.round = std::round(1.5 * S);
                add(makeUnique<CTexPassElement>(std::move(picture)));
            } else
                addRect(SCREEN, CHyprColor{0.3F, 0.3F, 0.3F, 0.9F * SHOWN}, std::round(1.5 * S));
            for (const auto& window : tile.windows)
                addRect(toPixels(window, monitor), CHyprColor{0.85F, 0.85F, 0.85F, 0.8F * SHOWN}, std::round(S));

            addOutline(monitor, TILE, tile.current ? ACCENT : CHyprColor{1.F, 1.F, 1.F, tile.hovered ? 0.7F : 0.15F}, tile.current ? 2 : 1, ROUND);

            if (refresh(g_spaceNames[i], tile.name, std::round(10 * S), std::round(TILE.w + 4 * S), CHyprColor{1.F, 1.F, 1.F, 0.8F}, false))
                addTexture(g_spaceNames[i].tex, Vector2D{TILE.x + (TILE.w - g_spaceNames[i].tex->m_size.x) / 2, TILE.y + TILE.h + 7 * S}.round());
        }
    }

    // At 100%, as on the Mac, the minimap and workspaces bar are on the
    // desktop, under the windows. Hyprland only redraws what changed, so when
    // what they show changes, so does their spot on screen.
    static std::unordered_map<std::string, size_t> g_desktopMaps; // per monitor, what was last drawn

    static size_t mapsHash(const std::optional<SMinimap>& map, const std::optional<SSpacesBar>& bar) {
        size_t     hash = 0;
        const auto MIX  = [&](double value) { hash = hash * 1000003 ^ std::hash<long long>{}(std::llround(value * 4)); };
        const auto BOX  = [&](const CBox& box) {
            MIX(box.x);
            MIX(box.y);
            MIX(box.w);
            MIX(box.h);
        };
        if (map) {
            BOX(map->box);
            BOX(map->viewport);
            for (const auto& window : map->windows)
                BOX(window);
        }
        if (bar)
            for (const auto& tile : bar->tiles) {
                BOX(tile.box);
                MIX(tile.current * 2 + tile.hovered);
                for (const auto& window : tile.windows)
                    BOX(window);
            }
        return hash;
    }

    void beforeFrame(const PHLMONITOR& monitor) {
        const auto MAP  = g_canvas.desktopMinimap(monitor);
        const auto BAR  = MAP ? g_canvas.desktopSpacesBar(monitor) : std::nullopt;
        const auto HASH = mapsHash(MAP, BAR);
        auto&      last = g_desktopMaps[monitor->m_name];
        if (last == HASH)
            return;
        last = HASH;
        // The panels sit in the bottom-right corner, the bar however wide it is.
        const CBox AREA = {monitor->m_position.x, monitor->m_position.y + monitor->m_size.y - 220, monitor->m_size.x, 220};
        g_pHyprRenderer->damageBox(AREA);
    }

    static void drawDesktopMaps(const PHLMONITOR& monitor) {
        const auto MAP = g_canvas.desktopMinimap(monitor);
        if (!MAP)
            return;
        addMinimap(monitor, *MAP, 1.0);
        if (const auto BAR = g_canvas.desktopSpacesBar(monitor))
            addSpacesBar(monitor, *BAR, false);
    }

    // Other monitors. A canvas reaches past its monitor's edges, over its
    // neighbours': a window there is on another canvas, off its screen. It
    // isn't drawn on the neighbour, and while the pointer is there it takes
    // no clicks or pointer focus (keyboard focus still reaches it).
    constexpr uint32_t INPUT_BLOCK_PARKED = 1U << 31; // well past Hyprland's own reasons

    static bool onAnotherCanvas(const PHLWINDOW& window, const PHLMONITOR& monitor) {
        if (!window->m_isMapped || !window->m_isFloating || window->m_pinned || window->m_monitorMovedFrom != -1 || !window->m_workspace ||
            !Mode::isExpanse(window->m_workspace) || !window->m_workspace->isVisible())
            return false;
        const auto OWN = window->m_monitor.lock();
        if (!OWN || OWN == monitor)
            return false;
        // A window you're dragging between monitors shows wherever it is.
        const auto DRAGGED = g_layoutManager->dragController()->target();
        return !(DRAGGED && DRAGGED == window->layoutTarget());
    }

    static CBox realBox(const PHLWINDOW& window) {
        return {window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    }

    static void hideForeignWindows(const PHLMONITOR& monitor) {
        if (g_canvas.paused())
            return;
        const CBox SCREEN = {monitor->m_position, monitor->m_size};
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window->isHidden() && onAnotherCanvas(window, monitor) && !realBox(window).intersection(SCREEN).empty()) {
                setHiddenQuietly(window, true);
                g_frame.foreign.push_back(window);
            }
        }
    }

    static void unhideForeignWindows() {
        for (const auto& window : g_frame.foreign)
            setHiddenQuietly(window, false);
        g_frame.foreign.clear();
    }

    void updateParking(bool active, const Vector2D& pointer) {
        PHLMONITOR UNDER;
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && monitor->m_enabled && CBox{monitor->m_position, monitor->m_size}.containsPoint(pointer))
                UNDER = monitor;
        for (const auto& window : Desktop::windowState()->windows()) {
            const bool PARK = active && UNDER && !Fullscreen::controller()->isFullscreen(window) && onAnotherCanvas(window, UNDER) &&
                !realBox(window).intersection({UNDER->m_position, UNDER->m_size}).empty();
            auto& reasons = window.get()->*robbed(SInputBlockTag{});
            reasons       = PARK ? reasons | INPUT_BLOCK_PARKED : reasons & ~INPUT_BLOCK_PARKED;
        }
    }

    // A window closed on the canvas just goes, as its picture does on the
    // Mac: Hyprland would fade it out at its real size and spot.
    static void dropCanvasFadeouts() {
        const auto WORKSPACE = g_canvas.workspace();
        auto&      fadeouts  = Desktop::fadingOutState().get()->*robbed(SFadeoutsTag{});
        std::erase_if(fadeouts, [&](const SP<Desktop::IFadeout>& fadeout) {
            const auto PLANE = fadeout ? fadeout->plane() : Desktop::FADEOUT_PLANE_POPUP;
            return fadeout && fadeout->workspace().lock() == WORKSPACE &&
                (PLANE == Desktop::FADEOUT_PLANE_WINDOW_TILED || PLANE == Desktop::FADEOUT_PLANE_WINDOW_FLOATING || PLANE == Desktop::FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN);
        });
    }

    static void hideCanvasWindows(PHLMONITOR monitor) {
        if (!g_canvas.drawsOn(monitor) || monitor->m_activeWorkspace != g_canvas.workspace())
            return;
        if (!Desktop::fadingOutState()->fadeouts().empty())
            dropCanvasFadeouts();

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

    // Labels are white with a shadow over the desktop picture, grey without:
    // turning it on or off makes them all again.
    static bool plainCanvas() {
        static bool was   = false;
        const bool  PLAIN = !g_canvas.wallpaper();
        if (PLAIN != was) {
            g_labels.clear();
            was = PLAIN;
            prepareLabels();
        }
        return PLAIN;
    }

    // Every window's name, the zoom's digits and the selection's dashes,
    // made ahead while nothing moves, a few at a time, so zooming out never
    // waits on making them.
    static wl_event_source* g_prepareTimer = nullptr;

    static bool prepareSome() {
        pruneLabels();
        const bool PLAIN = plainCanvas();
        const auto COLOR = labelColor(PLAIN);
        int        made  = 0;
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && monitor->m_enabled) {
                digits(std::round(10 * monitor->m_scale));
                dashes(monitor->m_scale);
            }
        for (const auto& window : Desktop::windowState()->windows()) {
            const auto MONITOR = window->m_monitor.lock();
            if (!window->m_isMapped || !MONITOR || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
                continue;
            const std::string NAME   = appName(window);
            const int         PX     = std::round(LABEL_PT * MONITOR->m_scale);
            auto&             cached = g_labels[window.get()];
            if (NAME.empty() || (cached.text == NAME && cached.px == PX && cached.color == COLOR))
                continue;
            if (made++ == LABELS_PER_FRAME)
                return true;
            refresh(cached, NAME, PX, 0, COLOR, !PLAIN);
        }
        return false;
    }

    void prepareLabels() {
        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!g_prepareTimer)
            g_prepareTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void*) {
                    // Up, the canvas makes what it lacks itself; closing it asks again.
                    if (g_canvas.live())
                        return 0;
                    if (::Render::GL::g_pHyprOpenGL) {
                        ::Render::GL::g_pHyprOpenGL->makeEGLCurrent();
                        if (prepareSome())
                            wl_event_source_timer_update(g_prepareTimer, 16);
                    }
                    return 0;
                },
                nullptr);
        wl_event_source_timer_update(g_prepareTimer, 100);
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

        // Over the desktop picture, dimmed; or, with it turned off, a plain
        // background (as Expanse's "Desktop Picture Background" menu item).
        const bool PLAIN = plainCanvas();
        // As on the Mac, the desktop picture as it is; or the plain background.
        const float SHOWN = sc<float>(g_canvas.zoomedOut());
        if (SHOWN > 0.F && PLAIN)
            addRect({0, 0, MONITOR->m_transformedSize.x, MONITOR->m_transformedSize.y}, (g_canvas.lightTheme() ? PLAIN_LIGHT : PLAIN_DARK).modifyA(SHOWN));

        // Only what's on this monitor (shadows reach past their windows).
        const CBox SCREEN = CBox{MONITOR->m_position, MONITOR->m_size}.expand((SHADOW_RADIUS + SHADOW_OFFSET) * g_canvas.scale() + 1);
        g_labelsMade      = 0;
        for (size_t i = 0; i < WINDOWS.size(); ++i) {
            const auto& window = WINDOWS[i];
            if (screens[i].intersection(SCREEN).empty())
                continue;
            auto& size = window->sizeAnimation();

            g_frame.restore.push_back({window, window->positionAnimation()->value(), size->value(), window->m_animatingIn, size->isBeingAnimated()});

            window->positionAnimation()->value() = screens[i].pos() - window->m_floatingOffset;
            size->value()                        = screens[i].size();
            // Scale the surfaces into that box instead of cropping them to it.
            window->m_animatingIn                = true;
            (*size).*robbed(SBeingAnimatedTag{}) = true;

            addShadow(MONITOR, window, screens[i], g_canvas.scale(), SHOWN);
            renderWindow(window, MONITOR, g_frame.now);
            addWindowBorder(MONITOR, window, screens[i], g_canvas.scale());

            if (!DECORATED)
                continue;

            // Details stay the same size on screen at any zoom; the ring covers the border.
            const bool SELECTED = g_canvas.isSelected(window);
            if (SELECTED || window == HOVER)
                addBorder(toPixels(screens[i], MONITOR), ACCENT, SELECTED ? 3 : 2, std::round(window->rounding() * g_canvas.scale() * MONITOR->m_scale));
            if (closes[i])
                addCloseButton(MONITOR, screens[i]);
            addLabel(MONITOR, window, screens[i], window == HOVER, PLAIN);
        }

        if (!DECORATED)
            return;

        if (SELECTIONBOUNDS) {
            const CBox SCREEN{g_canvas.toScreen(SELECTIONBOUNDS->pos()), SELECTIONBOUNDS->size() * g_canvas.scale()};
            addDashedOutline(toPixels(SCREEN.copy().expand(8), MONITOR), MONITOR->m_scale);
        }

        for (const auto& guide : GUIDES)
            addGuide(MONITOR, guide);

        if (MARQUEE) {
            const CBox PIXELS = toPixels(*MARQUEE, MONITOR);
            addRect(PIXELS, ACCENT.modifyA(0.12F));
            addBorder(PIXELS, ACCENT, 1, 0);
        }

        if (MINIMAP)
            addMinimap(MONITOR, *MINIMAP, g_canvas.scale());
        if (SPACESBAR)
            addSpacesBar(MONITOR, *SPACESBAR, PLAIN);
    }

    static void stageWork(eRenderStage stage);

    void onStage(eRenderStage stage) {
        stageWork(stage);
        if (stage == RENDER_POST)
            Perf::frameEnd();
    }

    static void stageWork(eRenderStage stage) {
        Perf::CPluginTime timed;
        switch (stage) {
            case RENDER_BEGIN:
                g_rendering = g_pHyprRenderer->m_renderData.pMonitor;
                if (const auto MONITOR = g_rendering.lock()) {
                    hideForeignWindows(MONITOR);
                    hideCanvasWindows(MONITOR);
                }
                break;
            case RENDER_PRE_WINDOWS:
                if (const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock())
                    drawDesktopMaps(MONITOR);
                break;
            // Below the bar and other top layers, above the wallpaper.
            case RENDER_POST_WINDOWS: drawCanvas(); break;
            // A frame that skipped the workspace (no damage) still gets its
            // windows back.
            case RENDER_LAST_MOMENT:
                unhideCanvasWindows();
                unhideForeignWindows();
                if (const auto MONITOR = g_rendering.lock()) {
                    Toast::draw(MONITOR);
                    // While something moves, the next frame. Asked for now,
                    // while this one draws, Hyprland keeps the request and
                    // schedules it once this frame is out; from render.pre,
                    // before the frame starts, it's dropped.
                    if (g_canvas.takeFrameRequest(MONITOR))
                        g_pHyprRenderer->damageMonitor(MONITOR);
                    Toast::afterFrame(MONITOR);
                }
                break;
            // The pass has drawn; the windows get their real geometry back.
            case RENDER_POST: {
                unhideCanvasWindows();
                unhideForeignWindows();
                restoreGeometry();
                g_rendering.reset();
                static bool wasLive = false;
                if (wasLive && !g_canvas.live())
                    prepareLabels(); // hovered names back to plain ones, and any missing
                wasLive = g_canvas.live();
                break;
            }
            default: break;
        }
    }

    void shutdown() {
        // Hyprland keeps the last frame's pass elements until the next frame
        // starts. The ones made here were made with deleters (and, for the
        // shadow, code) that go away with the plugin, so they go first. The
        // pass is rebuilt every frame and nothing is drawing now.
        g_pHyprRenderer->m_renderPass.clear();
        if (g_prepareTimer)
            wl_event_source_remove(g_prepareTimer);
        g_prepareTimer = nullptr;
        updateParking(false, {});
        unhideForeignWindows();
        unhideCanvasWindows();
        restoreGeometry();
        g_labels.clear();
        g_closeGlyph = {};
        g_digits.clear();
        g_dashes = {};
        g_desktopMaps.clear();
        Toast::shutdown();
        g_spacesHeading = {};
        g_spaceNames.clear();
    }
}
