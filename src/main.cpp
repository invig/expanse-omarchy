#define WLR_USE_UNSTABLE

#include <format>
#include <sstream>
#include <string>
#include <vector>

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <lua.hpp>

#include "Canvas.hpp"
#include "Render.hpp"
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
        HyprlandAPI::addNotification(PHANDLE, g_canvas.autoCentre() ? "Auto-centre on" : "Auto-centre off: clicking a window partly on screen leaves the canvas where it is",
                                     CHyprColor{0.54F, 0.71F, 0.98F, 1.F}, 2500);
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

static constexpr const char* USAGE = "usage: hyprctl expanse [state | toggle | open | close | hop <left|right|up|down> | pick <x> <y> | tidy | fit | zoom <scale> | "
                                     "pan <dx> <dy> | select all | autocentre <on|off>]\n";

static std::string ctlCommand(eHyprCtlOutputFormat, std::string request) {
    std::istringstream words{request};
    std::string        name, verb;
    words >> name >> verb;

    if (verb.empty() || verb == "state")
        ;
    else if (verb == "toggle")
        g_canvas.toggle();
    else if (verb == "open")
        g_canvas.open();
    else if (verb == "close")
        g_canvas.close();
    else if (verb == "tidy")
        g_canvas.tidy();
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
    } else if (verb == "select") {
        g_canvas.selectAll();
    } else if (verb == "autocentre" || verb == "autocenter") {
        std::string word;
        words >> word;
        if (word != "on" && word != "off")
            return USAGE;
        g_canvas.setAutoCentre(word == "on");
    } else
        return USAGE;

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

    auto& events = Event::bus()->m_events;
    g_listeners.push_back(events.render.pre.listen([](PHLMONITOR monitor) { g_canvas.onPreRender(monitor); }));
    g_listeners.push_back(events.render.stage.listen([](eRenderStage stage) { Expanse::Render::onStage(stage); }));
    g_listeners.push_back(events.input.mouse.move.listen([CANCEL](Vector2D pos, Event::SCallbackInfo& info) { CANCEL(g_canvas.onMouseMove(pos), info); }));
    g_listeners.push_back(events.input.mouse.button.listen(
        [CANCEL](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onMouseButton(event.button, event.state == WL_POINTER_BUTTON_STATE_PRESSED), info); }));
    g_listeners.push_back(events.input.mouse.axis.listen([CANCEL](IPointer::SAxisEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onAxis(event), info); }));
    g_listeners.push_back(events.gesture.pinch.begin.listen([CANCEL](IPointer::SPinchBeginEvent, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchBegin(), info); }));
    g_listeners.push_back(
        events.gesture.pinch.update.listen([CANCEL](IPointer::SPinchUpdateEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchUpdate(event.scale), info); }));
    g_listeners.push_back(events.gesture.pinch.end.listen([CANCEL](IPointer::SPinchEndEvent, Event::SCallbackInfo& info) { CANCEL(g_canvas.onPinchEnd(), info); }));
    g_listeners.push_back(events.input.keyboard.key.listen(
        [CANCEL](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) { CANCEL(g_canvas.onKey(event.keycode, event.state == WL_KEYBOARD_KEY_STATE_PRESSED), info); }));
    g_listeners.push_back(events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason reason) { g_canvas.onFocus(window, reason); }));
    g_listeners.push_back(events.window.openLate.listen([](PHLWINDOW window) { g_canvas.onWindowOpen(window); }));

    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "overview", luaOverview);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "hop", luaHop);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "tidy", luaTidy);
    HyprlandAPI::addLuaFunction(PHANDLE, "expanse", "auto_centre", luaAutoCentre);
    HyprlandAPI::addDispatcherV2(PHANDLE, "expanse:overview", [](std::string) {
        g_canvas.toggle();
        return SDispatchResult{};
    });
    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "expanse", .exact = false, .fn = ctlCommand});

    return {"expanse", "Expanse: an infinite, zoomable canvas for your windows", "Matthew Salmon", "0.2.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    Expanse::Render::shutdown();
    g_canvas.reset();
    g_listeners.clear();
    if (g_ctlCommand)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_ctlCommand);
    g_ctlCommand.reset();
}
