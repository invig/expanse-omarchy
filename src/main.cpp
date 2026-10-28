#define WLR_USE_UNSTABLE

#include <algorithm>
#include <chrono>
#include <cctype>
#include <format>
#include <sstream>
#include <string>
#include <vector>

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <lua.hpp>

#include "Canvas.hpp"
#include "HotCorner.hpp"
#include "Later.hpp"
#include "Perf.hpp"
#include "Render.hpp"
#include "Toast.hpp"
#include "globals.hpp"

using namespace Expanse;

static std::vector<CHyprSignalListener> g_listeners;
static SP<SHyprCtlCommand>              g_ctlCommand;

// Lua: each function returns its action, as hl.dsp.* do, for hl.bind and
// hyprctl dispatch alike.

static int luaPush(lua_State* L, lua_CFunction action) {
    lua_pushcfunction(L, action);
    return 1;
}

static int luaOverview(lua_State* L) {
    return luaPush(L, [](lua_State*) {
        g_canvas.toggle();
        return 0;
    });
}

static int luaTidy(lua_State* L) {
    return luaPush(L, [](lua_State*) {
        g_canvas.tidy();
        return 0;
    });
}

static int luaAutoCentre(lua_State* L) {
    return luaPush(L, [](lua_State*) {
        g_canvas.setAutoCentre(!g_canvas.autoCentre());
        Toast::show(g_canvas.autoCentre() ? "Auto-centre on" : "Auto-centre off: windows stay put");
        return 0;
    });
}

static int luaPause(lua_State* L) {
    return luaPush(L, [](lua_State*) {
        if (g_canvas.paused())
            g_canvas.resume();
        else
            g_canvas.pause();
        Toast::show(g_canvas.paused() ? "Expanse paused: windows are back where they were" : "Expanse resumed");
        return 0;
    });
}

static int luaRestore(lua_State* L) {
    return luaPush(L, [](lua_State*) {
        g_canvas.restoreOffscreen();
        return 0;
    });
}

static int luaHop(lua_State* L) {
    const auto DIRECTION = parseDirection(luaL_checkstring(L, 1));
    if (!DIRECTION)
        return luaL_error(L, "hl.plugin.expanse.hop: expected \"left\", \"right\", \"up\" or \"down\"");
    lua_pushinteger(L, *DIRECTION);
    lua_pushcclosure(
        L,
        [](lua_State* L) {
            g_canvas.hop(sc<eDirection>(lua_tointeger(L, lua_upvalueindex(1))));
            return 0;
        },
        1);
    return 1;
}

static constexpr const char* USAGE =
    "usage: hyprctl expanse [state | toggle | open | close | hop <left|right|up|down> | focus <app> | pick <x> <y> | space <name> | tidy | fit | zoom <scale> | "
    "pan <dx> <dy> | select all | restore | pause [toggle] | resume | autocentre <on|off|toggle> | wallpaper <on|off|toggle|default> | "
    "corner [off|all|bottom|top_left|top_right|bottom_left|bottom_right|default] | keys | screen [<w> <h>] | pinch <scale>... | perf [start [gpu]|stop]]\n";

// The canvas's own keys (binds are in Omarchy's key guide), for the menu's Shortcuts.
static constexpr const char* KEYS = R"(Expanse
  Alt + `                  zoom out to every window, and back
  Super + arrows           glide to the nearest window that way
  rest in a bottom corner  zoom out (hyprctl expanse corner ...)

Zoomed out
  click a window           glide to it at 100%
  arrows, Return           move the highlight, dive into it
  scroll, middle-drag      pan;  Space + drag pans too
  Super + scroll, pinch    zoom around the pointer
  0  1  -  =               fit everything, 100%, zoom out, zoom in
  drag a window            move it (snaps; hold Super to stop snapping)
  drag empty canvas        select; Shift-click adds; Super + A selects all
  T                        tidy up
  Super + W, hover x       close the hovered or selected windows
  minimap, workspaces bar  click to go there
  Esc, right-click         clear the selection, then back to 100%
)";

static std::string ctlCommand(eHyprCtlOutputFormat, std::string request) {
    std::istringstream words{request};
    std::string        name, verb;
    words >> name >> verb;

    if (verb.empty() || verb == "state")
        ;
    else if (verb == "perf") {
        // Frame timing, to find jank: start, do something, stop for the frames as JSON.
        std::string word;
        words >> word;
        if (word == "start") {
            std::string gpu;
            words >> gpu;
            Perf::start(gpu == "gpu");
        }
        else if (word == "stop")
            return Perf::stop();
        else
            return std::format("perf: {}\n", Perf::recording() ? "recording" : "off");
        return "ok\n";
    } else if (verb == "toggle")
        g_canvas.toggle();
    else if (verb == "open")
        g_canvas.open();
    else if (verb == "close")
        g_canvas.close();
    else if (verb == "tidy")
        g_canvas.tidy();
    else if (verb == "restore")
        g_canvas.restoreOffscreen();
    else if (verb == "pause") {
        std::string word;
        words >> word;
        if (word == "toggle" && g_canvas.paused())
            g_canvas.resume();
        else
            g_canvas.pause();
    } else if (verb == "resume")
        g_canvas.resume();
    else if (verb == "keys")
        return KEYS;
    else if (verb == "corner") {
        // Which corners zoom out, as the Mac's menu: saved, and over the config value until "default".
        std::string word;
        words >> word;
        static const std::vector<std::string> CORNERS = {"off", "all", "bottom", "top_left", "top_right", "bottom_left", "bottom_right"};
        if (word == "default")
            g_canvas.setHotCorner(std::nullopt);
        else if (std::ranges::contains(CORNERS, word))
            g_canvas.setHotCorner(word);
        else if (!word.empty())
            return USAGE;
    } else if (verb == "wallpaper") {
        // The desktop picture behind the canvas, or a plain background; saved, as "corner".
        std::string word;
        words >> word;
        if (word == "on" || word == "off")
            g_canvas.setWallpaper(word == "on");
        else if (word == "toggle")
            g_canvas.setWallpaper(!g_canvas.wallpaper());
        else if (word == "default")
            g_canvas.setWallpaper(std::nullopt);
        else
            return USAGE;
    }
    else if (verb == "focus") {
        // The newest window whose app matches, as expanse://focus?app= on the
        // Mac; or, as ?id=, one window by its address (from hyprctl clients).
        std::string app;
        std::getline(words >> std::ws, app);
        const bool ADDRESS = app.starts_with("address:");
        const auto LOWER = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        };
        PHLWINDOW match;
        for (const auto& window : g_canvas.canvasWindows())
            if (ADDRESS ? app == std::format("address:0x{:x}", reinterpret_cast<uintptr_t>(window.get())) :
                          !app.empty() && (LOWER(window->m_class).contains(LOWER(app)) || LOWER(window->m_initialClass).contains(LOWER(app))))
                match = window;
        if (!match)
            return std::format("no window of {} on this canvas\n", app);
        g_canvas.glideTo(match);
    }
    else if (verb == "fit")
        g_canvas.fit();
    else if (verb == "hop") {
        std::string word;
        words >> word;
        const auto DIRECTION = parseDirection(word);
        if (!DIRECTION)
            return USAGE;
        g_canvas.hop(*DIRECTION);
    } else if (verb == "pick") {
        double x = 0, y = 0;
        if (!(words >> x >> y))
            return USAGE;
        g_canvas.pickAt({x, y});
    } else if (verb == "zoom") {
        double scale = 0;
        if (!(words >> scale) || scale <= 0)
            return USAGE;
        g_canvas.zoomTo(scale);
    } else if (verb == "pan") {
        double dx = 0, dy = 0;
        if (!(words >> dx >> dy))
            return USAGE;
        g_canvas.pan({-dx, -dy});
    } else if (verb == "screen") {
        // Debugging, as expanse://screen on the Mac: pretend the screen is
        // this size (no size: the real one again).
        double w = 0, h = 0;
        if (words >> w >> h && w > 0 && h > 0)
            g_canvas.simulateScreen(Vector2D{w, h});
        else
            g_canvas.simulateScreen(std::nullopt);
    } else if (verb == "space") {
        // As clicking its tile on the workspaces bar: that workspace's canvas, zoomed out.
        std::string name;
        std::getline(words >> std::ws, name);
        PHLWORKSPACE match;
        for (const auto& workspace : State::workspaceState()->workspaces())
            if (workspace && !workspace->inert() && !workspace->m_isSpecialWorkspace && workspace->m_name == name)
                match = workspace.lock();
        if (!match)
            return std::format("no workspace named {}\n", name);
        if (!g_canvas.switchTo(match))
            return std::format("workspace {} isn't on the canvas's monitor\n", name);
    } else if (verb == "pinch") {
        // Debugging: a two-finger pinch through Hyprland's own input path, as
        // a touchpad's arrives from libinput, through these scales.
        std::vector<double> scales;
        for (double scale = 0; words >> scale;)
            scales.push_back(scale);
        if (scales.empty())
            return USAGE;
        const auto NOW = [] { return sc<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        g_pInputManager->onPinchBegin({.timeMs = NOW(), .fingers = 2});
        for (const double scale : scales)
            g_pInputManager->onPinchUpdate({.timeMs = NOW(), .fingers = 2, .scale = scale});
        g_pInputManager->onPinchEnd({.timeMs = NOW()});
    } else if (verb == "select") {
        g_canvas.selectAll();
    } else if (verb == "autocentre" || verb == "autocenter") {
        std::string word;
        words >> word;
        if (word == "toggle")
            g_canvas.setAutoCentre(!g_canvas.autoCentre());
        else if (word == "on" || word == "off")
            g_canvas.setAutoCentre(word == "on");
        else
            return USAGE;
        // As the Mac's menu item does.
        Toast::show(g_canvas.autoCentre() ? "Auto-centre on" : "Auto-centre off: windows stay put");
    } else
        return USAGE;

    if (verb == "corner")
        return g_hotCorner.describe() + "\n";
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

    const auto CANCEL = [](bool swallow, Event::SCallbackInfo& info) {
        if (swallow)
            info.cancelled = true;
    };

    HyprlandAPI::addConfigValueV2(PHANDLE,
                                  makeShared<Config::Values::CBoolValue>("plugin:expanse:wallpaper", "the desktop picture behind the canvas; off: a plain background",
                                                                         Config::BOOL{true}));
    HyprlandAPI::addConfigValueV2(PHANDLE,
                                  makeShared<Config::Values::CStringValue>("plugin:expanse:hot_corner",
                                                                           "rest the pointer here to zoom out: off, all, bottom, top_left, top_right, bottom_left or bottom_right",
                                                                           Config::STRING{"bottom"}));
    g_hotCorner.onTrigger = [] { g_canvas.toggle(); };

    auto& events = Event::bus()->m_events;
    g_listeners.push_back(events.render.pre.listen([](PHLMONITOR monitor) {
        Perf::frameStart(monitor);
        Perf::CPluginTime timed;
        Expanse::Render::updateParking(!g_canvas.paused(), g_pInputManager->getMouseCoordsInternal());
        g_canvas.onPreRender(monitor);
        Expanse::Render::beforeFrame(monitor);
        // While the canvas is up it swallows pointer moves, so Hyprland's
        // record of the last one stays put and it doesn't report moving back
        // to that spot; the corner reads the pointer each frame instead.
        if (g_canvas.live())
            g_hotCorner.onMouseMove(g_pInputManager->getMouseCoordsInternal());
    }));
    g_listeners.push_back(events.render.stage.listen([](eRenderStage stage) { Expanse::Render::onStage(stage); }));
    g_listeners.push_back(events.input.mouse.move.listen([CANCEL](Vector2D pos, Event::SCallbackInfo& info) {
        // Before Hyprland picks the window under the pointer.
        Expanse::Render::updateParking(!g_canvas.paused(), pos);
        g_hotCorner.onMouseMove(pos);
        CANCEL(g_canvas.onMouseMove(pos), info);
    }));
    g_listeners.push_back(events.input.mouse.button.listen([CANCEL](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        g_hotCorner.onButton(event.state == WL_POINTER_BUTTON_STATE_PRESSED);
        CANCEL(g_canvas.onMouseButton(event.button, event.state == WL_POINTER_BUTTON_STATE_PRESSED), info);
    }));
    g_listeners.push_back(events.input.mouse.axis.listen([CANCEL](IPointer::SAxisEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onAxis(event), info); }));
    g_listeners.push_back(events.gesture.pinch.begin.listen([CANCEL](IPointer::SPinchBeginEvent, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchBegin(), info); }));
    g_listeners.push_back(
        events.gesture.pinch.update.listen([CANCEL](IPointer::SPinchUpdateEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchUpdate(event.scale), info); }));
    g_listeners.push_back(events.gesture.pinch.end.listen([CANCEL](IPointer::SPinchEndEvent, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchEnd(), info); }));
    g_listeners.push_back(events.input.keyboard.key.listen(
        [CANCEL](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onKey(event.keycode, event.state == WL_KEYBOARD_KEY_STATE_PRESSED), info); }));
    g_listeners.push_back(events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason reason) { g_canvas.onFocus(window, reason); }));
    g_listeners.push_back(events.window.openLate.listen([](PHLWINDOW window) {
        g_canvas.onWindowOpen(window);
        g_canvas.damageDesktopMaps();
        Expanse::Render::prepareLabels();
    }));
    g_listeners.push_back(events.window.close.listen([](PHLWINDOW window) {
        g_canvas.onWindowClose(window);
        g_canvas.damageDesktopMaps();
    }));
    g_listeners.push_back(events.window.fullscreen.listen([](PHLWINDOW window) { g_canvas.onFullscreen(window); }));
    g_listeners.push_back(events.monitor.layoutChanged.listen([] {
        g_hotCorner.disarm();
        g_canvas.onScreenChanged();
        Expanse::Render::prepareLabels(); // a new scale, new sizes
    }));
    for (auto* event : {&events.monitor.preAdded, &events.monitor.added, &events.monitor.preRemoved, &events.monitor.removed})
        g_listeners.push_back(event->listen([](PHLMONITOR) {
            g_hotCorner.disarm();
            g_canvas.onScreenChanged();
        }));
    g_listeners.push_back(events.workspace.active.listen([](PHLWORKSPACE workspace) {
        g_canvas.onWorkspaceChanged(workspace);
        g_canvas.damageDesktopMaps();
    }));

    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "overview", luaOverview);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "hop", luaHop);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "tidy", luaTidy);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "auto_centre", luaAutoCentre);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "pause", luaPause);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "restore", luaRestore);
    HyprlandAPI::addDispatcherV2(PHANDLE, "expanse:overview", [](std::string) {
        g_canvas.toggle();
        return SDispatchResult{};
    });
    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "expanse", .exact = false, .fn = ctlCommand});

    // The canvas comes back where it was, once Hyprland has finished loading us.
    later([] {
        g_canvas.restoreCanvas();
        Expanse::Render::prepareLabels();
    });

    return {"expanse", "Expanse: an infinite, zoomable canvas for your windows", "Matthew Salmon", "0.3.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Nothing of ours may run once the plugin is gone: no more events first
    // (putting windows back raises some), then nothing left waiting.
    g_listeners.clear();
    g_hotCorner.stop();
    Expanse::Render::shutdown();
    // As quitting does on the Mac: the layout is saved, and windows go back.
    g_canvas.shutdown();
    cancelLater();
    if (g_ctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctlCommand);
    g_ctlCommand.reset();
}
