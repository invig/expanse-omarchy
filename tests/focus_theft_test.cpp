// Focus theft through a client-chosen app id while the canvas is zoomed out.
//
// These assertions are the behaviour after the fix: a normal toplevel is not
// treated as a password prompt because its app id contains "polkit" or
// "pinentry", or equals "gcr-prompter". A real dialog (a modal, or a child
// of another window) still makes the canvas step aside.
//
// The cases that describe a spoofed app id fail while that classification
// is still in isDialog. Run: make focus-theft-test
//
// Hyprland sources cited below are v0.56.2, commit efb50993780079460b0cbed1363e2166a2de1d9f,
// the version docs/feasibility.md records this plugin against. hyprutils boxes
// are revision 5a7b8cf, the one that tag's flake.lock pins.

#include "../src/Canvas.hpp"
#include "../src/Later.hpp"
#include "../src/Mode.hpp"

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <linux/input-event-codes.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Expanse;

static int failures = 0;
static int checks   = 0;

static void expect(bool ok, const char* name) {
    ++checks;
    if (ok)
        return;
    ++failures;
    std::printf("FAIL %s\n", name);
}

static PHLMONITOR                 g_monitor;
static PHLWORKSPACE               g_workspace;
static SP<Layout::CSpace>         g_space;

static void installExpanseLayout() {
    // Mode.cpp registers CExpanseAlgorithm through HyprlandAPI::addTiledAlgo.
    // isExpanse() is true only when that algorithm is the workspace's tiled one.
    Mode::add(nullptr);
    auto algorithm = std::make_shared<Layout::CAlgorithm>();
    algorithm->setTiled(HyprlandAPI::g_tiledFactory());
    g_space             = std::make_shared<Layout::CSpace>();
    g_space->m_algorithm = algorithm;
    g_space->m_work      = {0, 0, 1920, 1080};

    g_monitor                    = std::make_shared<CMonitor>();
    g_monitor->m_position        = {0, 0};
    g_monitor->m_size            = {1920, 1080};
    g_monitor->m_name            = "eDP-1";
    g_monitor->m_enabled         = true;
    g_workspace                  = std::make_shared<CWorkspace>();
    g_workspace->m_name          = "1";
    g_workspace->m_id            = 1;
    g_workspace->m_visible       = true;
    g_workspace->m_monitor       = g_monitor;
    g_workspace->m_space         = g_space;
    g_space->m_workspace         = g_workspace;
    g_monitor->m_activeWorkspace = g_workspace;

    State::monitorState()->m_monitors   = {g_monitor};
    State::workspaceState()->m_workspaces = {g_workspace};
    Desktop::focusState()->m_focusMonitor = g_monitor;

    if (!g_pCompositor->m_wlEventLoop)
        g_pCompositor->m_wlEventLoop = wl_event_loop_create();
    if (!g_pSeatManager->m_keyboard)
        g_pSeatManager->m_keyboard = std::make_shared<IKeyboard>();
}

// XDGShell.cpp:188-190 stores the client string with no check.
// Window.cpp:1502-1505 and 2081 copy it into m_initialClass at map, before openLate (line 2555).
static void setWaylandAppId(const PHLWINDOW& window, const std::string& appId) {
    window->m_isX11       = false;
    window->m_xdgSurface  = std::make_shared<CXDGSurfaceResource>();
    window->m_xdgSurface->m_toplevel = std::make_shared<CXDGToplevelResource>();
    window->m_xdgSurface->m_toplevel->m_window = window;
    window->m_xdgSurface->m_toplevel->m_state.appid = appId;
    window->m_initialClass = window->fetchClass();
    window->m_class        = window->m_initialClass;
}

// XWM.cpp:221-227. WM_CLASS is instance\0class\0. Two interior NULs: keep the
// class (the second string) and drop its trailing NUL. fetchClass returns that.
static std::string x11ClassFromProperty(const std::string& value) {
    std::string appid = value;
    if (std::count(appid.begin(), appid.end(), '\000') == 2)
        appid = appid.substr(appid.find_first_of('\000') + 1);
    if (!appid.empty())
        appid.pop_back();
    return appid;
}

static void setX11Class(const PHLWINDOW& window, const std::string& wmClassProperty, bool overrideRedirect, bool modal) {
    window->m_isX11            = true;
    window->m_xwaylandSurface  = std::make_shared<CXWaylandSurface>();
    window->m_xwaylandSurface->m_overrideRedirect = overrideRedirect;
    window->m_xwaylandSurface->m_modal            = modal;
    window->m_xwaylandSurface->m_state.appid      = x11ClassFromProperty(wmClassProperty);
    window->m_initialClass = window->fetchClass();
    window->m_class        = window->m_initialClass;
}

static int g_nextClient = 1;

static PHLWINDOW makeWindow() {
    auto window       = std::make_shared<CWindow>();
    window->m_isMapped = true;
    window->m_pid      = 1000 + g_nextClient;
    window->m_pos      = {100, 100};
    window->m_size     = {400, 300};
    window->m_workspace = g_workspace;
    window->m_monitor   = g_monitor;

    window->m_surface         = std::make_shared<CWLSurfaceResource>();
    window->m_surface->owner  = std::make_shared<CClient>();
    window->m_surface->owner->id = g_nextClient++;
    window->m_wl.res          = window->m_surface;

    auto keyboard      = std::make_shared<CSeatKeyboard>();
    keyboard->owner    = window->m_surface->owner;
    auto seat          = std::make_shared<CWLSeatResource>();
    seat->owner        = window->m_surface->owner;
    seat->m_keyboards  = {keyboard};
    g_pSeatManager->m_seatResources.push_back(seat);

    window->m_target           = std::make_shared<CWindowTarget>();
    window->m_target->m_window = window;
    window->m_target->m_space  = g_space;
    window->m_target->m_floating = true;
    window->m_target->m_box    = {window->m_pos, window->m_size};

    Desktop::windowState()->m_windows.push_back(window);
    return window;
}

static void clearWindows() {
    Desktop::windowState()->m_windows.clear();
    g_pSeatManager->m_seatResources.clear();
    g_pSeatManager->m_state = {};
    Desktop::focusState()->m_focusWindow.reset();
    if (g_pEventLoopManager)
        g_pEventLoopManager->queue.clear();
    g_canvas.reset();
}

// main.cpp:456-457 cancels the key when onKey returns true.
// InputManager.cpp:1630-1631 then returns without sending.
// KeybindManager.cpp:842 passes the event when no bind matched (the bind list
// here is empty, so an unmodified key is not a bind). handleInternalKeybinds
// (line 928) only swallows VT switches and Escape-in-kill-mode.
// SeatManager.cpp:255 sends the key to the focused client's keyboard.
static bool deliverUnboundKey(uint32_t keycode) {
    // main.cpp:411-413 and 456-457: onKey returning true cancels the event.
    bool cancelled = g_canvas.onKey(keycode, true);
    // InputManager.cpp:1630-1631.
    if (cancelled)
        return false;
    // KeybindManager.cpp:605 and 842. No bind matches, so the event is passed.
    bool       found     = false;
    const bool passEvent = !found;
    if (!passEvent)
        return false;
    g_pSeatManager->sendKeyboardKey(0, keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
    return true;
}

static void pumpAnimation() {
    // animateTo arms a wayland timer (Canvas.cpp). wl_event_loop_dispatch
    // returns when the first source fires (Wayland 1.22 event-loop.c), so a
    // pending save timer can return before the glide timer. Repeat short
    // waits so the glide still lands, without sitting for the 1000 ms save.
    for (int i = 0; i < 5; ++i)
        wl_event_loop_dispatch(g_pCompositor->m_wlEventLoop, 200);
    flushLaters();
}

static bool zoomOut() {
    g_canvas.open(g_monitor);
    pumpAnimation();
    return g_canvas.showsCanvas();
}

static bool attackerGotKey(const PHLWINDOW& window, uint32_t key) {
    const auto CLIENT = window->m_surface->owner;
    return std::ranges::find(CLIENT->pressedKeys, key) != CLIENT->pressedKeys.end();
}

static void openWhileZoomed(const PHLWINDOW& window) {
    g_canvas.onWindowOpen(window);
    flushLaters();
    pumpAnimation();
    flushLaters();
}

// Secure outcome: this window must not end up with the keyboard.
static void expectNoFocusTheft(const char* name, const PHLWINDOW& attacker) {
    const bool focused = Desktop::focusState()->window() == attacker;
    const bool gotKey  = attackerGotKey(attacker, KEY_A);
    deliverUnboundKey(KEY_B);
    const bool gotNext = attackerGotKey(attacker, KEY_B);
    expect(!focused && !gotKey && !gotNext, name);
    if (focused || gotKey || gotNext)
        std::printf("  focused=%d keyA=%d keyB=%d class=%s\n", focused, gotKey, gotNext, attacker->m_initialClass.c_str());
}

// Secure outcome: the canvas does step aside for a window Hyprland already
// knows is a dialog, and the next unbound key is delivered to it.
static void expectDialogReceivesKey(const char* name, const PHLWINDOW& dialog) {
    openWhileZoomed(dialog);
    const bool focused = Desktop::focusState()->window() == dialog;
    const bool sent    = deliverUnboundKey(KEY_A);
    const bool gotKey  = attackerGotKey(dialog, KEY_A);
    expect(focused && sent && gotKey, name);
    if (!(focused && sent && gotKey))
        std::printf("  focused=%d sent=%d key=%d class=%s live=%d\n", focused, sent, gotKey, dialog->m_initialClass.c_str(), g_canvas.live());
}

int main() {
    setenv("XDG_STATE_HOME", "/tmp/expanse-focus-theft", 1);
    installExpanseLayout();
    flushLaters(); // the layout factory's one-shot clear

    // The canvas holds the keyboard while zoomed out: an unbound key is not delivered.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zooming out opens the canvas");
        expect(g_canvas.onKey(KEY_A, true), "zoomed out, the canvas swallows an unbound key");
        expect(!attackerGotKey(victim, KEY_A), "a swallowed key is not delivered to the focused window");
    }

    auto spoof = [&](const char* name, const std::string& appId) {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), name);
        auto attacker = makeWindow();
        setWaylandAppId(attacker, appId);
        openWhileZoomed(attacker);
        expectNoFocusTheft(name, attacker);
    };

    spoof("app id polkit is not a password agent", "polkit");
    spoof("app id containing polkit, any case, is not a password agent", "org.gnome.PolkitAgent");
    spoof("app id containing pinentry is not a password agent", "pinentry-gtk-2");
    spoof("app id gcr-prompter is not a password agent", "gcr-prompter");
    spoof("app id GCR-PROMPTER is not a password agent", "GCR-PROMPTER");

    // X11 WM_CLASS class string "polkit", not an override-redirect window.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before an x11 class");
        auto attacker = makeWindow();
        std::string prop = std::string("evil") + '\0' + "polkit" + '\0';
        setX11Class(attacker, prop, false, false);
        openWhileZoomed(attacker);
        expectNoFocusTheft("an x11 class of polkit is not a password agent", attacker);
    }

    // Glide already in progress: the spoofed window is focused when it lands.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before a glide");
        expect(g_canvas.onKey(KEY_ENTER, true), "enter starts a glide to the window in view");
        auto attacker = makeWindow();
        setWaylandAppId(attacker, "polkit");
        g_canvas.onWindowOpen(attacker);
        flushLaters();
        pumpAnimation();
        flushLaters();
        expectNoFocusTheft("a spoofed app id opened mid-glide is not focused when the glide lands", attacker);
    }

    // A normal app id does not take the keyboard. This passes with the bug.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before an ordinary window");
        auto other = makeWindow();
        setWaylandAppId(other, "firefox");
        openWhileZoomed(other);
        expectNoFocusTheft("an ordinary app id does not take focus", other);
        expect(g_canvas.showsCanvas(), "the canvas stays zoomed out for an ordinary window");
    }

    // Equality, not a substring, for gcr-prompter. Passes with the bug.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before a lookalike id");
        auto other = makeWindow();
        setWaylandAppId(other, "not-gcr-prompter");
        openWhileZoomed(other);
        expectNoFocusTheft("an app id that only contains gcr-prompter is not that agent", other);
    }

    // A child of another window is a dialog the compositor already knows about.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before a child dialog");
        auto dialog = makeWindow();
        setWaylandAppId(dialog, "some.normal.app");
        dialog->m_xdgSurface->m_toplevel->m_parent = victim->m_xdgSurface->m_toplevel;
        expect(dialog->parent() == victim, "the child window's parent is the window it named");
        expectDialogReceivesKey("a child of another window still receives the next key", dialog);
    }

    // X11 modal, which is what CWindow::isModal reads. Not an app id.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before an x11 modal");
        auto dialog = makeWindow();
        std::string prop = std::string("dialog") + '\0' + "attacker" + '\0';
        setX11Class(dialog, prop, false, true);
        expect(dialog->isModal(), "the x11 window is modal");
        expectDialogReceivesKey("an x11 modal still receives the next key", dialog);
    }

    // Override-redirect is excluded before the app id is read.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        expect(zoomOut(), "zoom out before an override-redirect window");
        auto popup = makeWindow();
        std::string prop = std::string("evil") + '\0' + "polkit" + '\0';
        setX11Class(popup, prop, true, false);
        openWhileZoomed(popup);
        expectNoFocusTheft("an override-redirect window is not focused", popup);
    }

    // Not zoomed out: the canvas is not holding the keyboard, and does not glide.
    {
        clearWindows();
        auto victim = makeWindow();
        setWaylandAppId(victim, "org.wezfurlong.wezterm");
        Desktop::focusState()->fullWindowFocus(victim, Desktop::FOCUS_REASON_NEW_WINDOW);
        auto attacker = makeWindow();
        setWaylandAppId(attacker, "polkit");
        g_canvas.onWindowOpen(attacker);
        flushLaters();
        pumpAnimation();
        flushLaters();
        expectNoFocusTheft("a spoofed app id while the canvas is idle does not take focus", attacker);
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
