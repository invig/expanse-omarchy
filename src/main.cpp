#define WLR_USE_UNSTABLE

#include <format>
#include <sstream>
#include <string>
#include <vector>

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>

#include <lua.hpp>

#include "Canvas.hpp"
#include "globals.hpp"

using namespace Expanse;

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
    using type = void (Render::IHyprRenderer::*)(PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
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

template struct SRob<SRenderWindowTag, &Render::IHyprRenderer::renderWindow>;
template struct SRob<SHiddenTag, &Desktop::View::CWindow::m_hidden>;
template struct SRob<SBeingAnimatedTag, &Hyprutils::Animation::CBaseAnimatedVariable::m_bIsBeingAnimated>;

// Without decorations: their boxes are laid out for the window's real size.
static void renderWindow(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now) {
    (g_pHyprRenderer.get()->*robbed(SRenderWindowTag{}))(window, monitor, now, false, Render::RENDER_PASS_ALL, false, false);
}

static void setHiddenQuietly(const PHLWINDOW& window, bool hidden) {
    window.get()->*robbed(SHiddenTag{}) = hidden;
}

static std::vector<CHyprSignalListener> g_listeners;
static SP<SHyprCtlCommand>              g_ctlCommand;

static const CHyprColor                 RING_COLOR = CHyprColor{0.54F, 0.71F, 0.98F, 1.F};
constexpr float                         RING_WIDTH = 3.F;
constexpr float                         DIM_ALPHA  = 0.45F;

// Hyprland's function hooks only work on x86_64, so the canvas draws from
// render-stage events. At the start of a frame the canvas windows are hidden
// from Hyprland's own pass; after its windows, each one is drawn where the
// camera puts it by giving it that box as its current (animated) geometry for
// the rest of the frame, the way Hyprland draws a window mid-animation.
// Hyprland clips a surface to its untransformed box, so render modifiers
// can't move a window that really sits off-screen.
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

static void addRing(PHLMONITOR monitor, PHLWINDOW window, const CBox& screen) {
    CBorderPassElement::SBorderData ring;
    ring.box        = screen.copy().translate(-monitor->m_position).scale(monitor->m_scale).round();
    ring.grad1      = Config::CGradientValueData{RING_COLOR};
    ring.borderSize = std::round(RING_WIDTH * monitor->m_scale);
    ring.round      = std::round(window->rounding() * monitor->m_scale);
    ring.outerRound = ring.round + ring.borderSize;
    ring.a          = sc<float>(g_canvas.zoomedOut());
    ring.window     = window;
    g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(ring));
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
        WINDOW->positionAnimation()->value()                            = saved.position;
        WINDOW->sizeAnimation()->value()                                = saved.size;
        WINDOW->m_animatingIn                                           = saved.animatingIn;
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

    const float DIM = DIM_ALPHA * sc<float>(g_canvas.zoomedOut());
    if (DIM > 0.F) {
        CRectPassElement::SRectData dim;
        dim.box   = {0, 0, MONITOR->m_transformedSize.x, MONITOR->m_transformedSize.y};
        dim.color = CHyprColor{0.F, 0.F, 0.F, DIM};
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(dim));
    }

    // Every box first: they're read from the geometry about to change.
    std::vector<CBox> screens;
    for (const auto& window : WINDOWS)
        screens.push_back(g_canvas.screenBox(window));

    const auto HOVER = g_canvas.hovered();
    for (size_t i = 0; i < WINDOWS.size(); ++i) {
        const auto& window = WINDOWS[i];
        auto&       size   = window->sizeAnimation();

        g_frame.restore.push_back({window, window->positionAnimation()->value(), size->value(), window->m_animatingIn, size->isBeingAnimated()});

        window->positionAnimation()->value() = screens[i].pos() - window->m_floatingOffset;
        size->value()                        = screens[i].size();
        // Scale the surfaces into that box instead of cropping them to it.
        window->m_animatingIn                  = true;
        (*size).*robbed(SBeingAnimatedTag{}) = true;

        renderWindow(window, MONITOR, g_frame.now);

        if (window == HOVER && g_canvas.zoomedOut() > 0.0)
            addRing(MONITOR, window, screens[i]);
    }
}

static void onRenderStage(eRenderStage stage) {
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
            break;
        default: break;
    }
}

static int luaToggle(lua_State*) {
    g_canvas.toggle();
    return 0;
}

// Returns the action, as hl.dsp.* do, for hl.bind and hyprctl dispatch.
static int luaOverview(lua_State* L) {
    lua_pushcfunction(L, luaToggle);
    return 1;
}

static std::string ctlCommand(eHyprCtlOutputFormat, std::string request) {
    std::istringstream words{request};
    std::string        name, verb;
    words >> name >> verb;

    if (verb == "toggle")
        g_canvas.toggle();
    else if (verb == "open" && !g_canvas.live())
        g_canvas.open();
    else if (verb == "close")
        g_canvas.close();
    else if (verb == "pick") {
        double x = 0, y = 0;
        if (!(words >> x >> y))
            return "usage: hyprctl expanse pick <x> <y>\n";
        g_canvas.pickAt({x, y});
    } else if (!verb.empty() && verb != "state")
        return "usage: hyprctl expanse [state|toggle|open|close|pick <x> <y>]\n";

    return g_canvas.stateJson();
}

static void fail(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE, "[expanse] " + reason, CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 8000);
    throw std::runtime_error("[expanse] " + reason);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        fail("built for a different Hyprland version; rebuild it (make, or hyprpm update)");

    auto& events = Event::bus()->m_events;
    g_listeners.push_back(events.render.pre.listen([](PHLMONITOR monitor) { g_canvas.onPreRender(monitor); }));
    g_listeners.push_back(events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); }));
    g_listeners.push_back(events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo& info) {
        if (g_canvas.onMouseMove(pos))
            info.cancelled = true;
    }));
    g_listeners.push_back(events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (g_canvas.onMouseButton(event.button, event.state == WL_POINTER_BUTTON_STATE_PRESSED))
            info.cancelled = true;
    }));
    g_listeners.push_back(events.input.mouse.axis.listen([](IPointer::SAxisEvent, Event::SCallbackInfo& info) {
        if (g_canvas.live())
            info.cancelled = true;
    }));
    g_listeners.push_back(events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (g_canvas.onKey(event.keycode, event.state == WL_KEYBOARD_KEY_STATE_PRESSED))
            info.cancelled = true;
    }));

    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "overview", luaOverview);
    HyprlandAPI::addDispatcherV2(PHANDLE, "expanse:overview", [](std::string) {
        g_canvas.toggle();
        return SDispatchResult{};
    });
    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "expanse", .exact = false, .fn = ctlCommand});

    return {"expanse", "Expanse: an infinite, zoomable canvas for your windows", "Matthew Salmon", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    unhideCanvasWindows();
    restoreGeometry();
    g_canvas.reset();
    g_listeners.clear();
    if (g_ctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctlCommand);
    g_ctlCommand.reset();
}
