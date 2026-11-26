// Spec for the zoomed-out key leak.
//
// While the canvas is up it is supposed to own the keyboard. Ordinary keys
// are swallowed. Chords that include Ctrl, Alt, or Super — other than the
// binds Expanse itself installs, and other than Super+A / Super+W which the
// canvas handles — are not. Hyprland then delivers an unbound chord to
// whatever surface still has keyboard focus. Zooming out does not move
// keyboard focus (CCanvas::open / begin never call fullWindowFocus unless a
// window is leaving fullscreen or being floated, and a window that is
// already floating is left alone).
//
// These cases fail until that is fixed: a Ctrl/Alt/Super chord pressed in
// the overview, while opening, or while diving back, must not reach the
// focused client. The binds in examples/expanse.lua must still reach
// Hyprland's keybind dispatcher. At 100%, and during a glide that is not a
// dive, the focused client still receives keys.
//
// The compositor steps below are the Hyprland 0.56.2 path, in order:
//   src/devices/Keyboard.cpp CKeyboard key listener (key, then xkb mods)
//   src/managers/input/InputManager.cpp CInputManager::onKeyboardKey
//   src/managers/input/InputManager.cpp CInputManager::onKeyboardMod
//   src/managers/SeatManager.cpp CSeatManager::sendKeyboardKey / sendKeyboardMods
//   src/managers/KeybindManager.cpp CKeybindManager::handleKeybinds
//       (passEvent is true when no consuming bind matched)
// and the plugin's listener:
//   src/main.cpp PLUGIN_INIT, the CANCEL lambda and input.keyboard.key

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Arrange.hpp"
#include "Nearest.hpp"
#include "Store.hpp"
#include "globals.hpp"
#include "hyprland_api.hpp"

// Access m_state / m_diving / m_swallowedKeys. The headers above are already
// included, so this does not rewrite private in the standard library.
#define private public
#include "Canvas.hpp"
#undef private

#include <linux/input-event-codes.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace Expanse;

namespace {

    int g_failures = 0;

    void expect(bool ok, const char* name) {
        if (ok)
            return;
        ++g_failures;
        std::printf("FAIL %s\n", name);
    }

    // examples/expanse.lua. A matching press is consuming, so Hyprland does
    // not also hand it to the client (handleKeybinds sets found, and
    // passEvent stays false for a consuming bind).
    struct SBind {
        uint32_t mods;
        uint32_t key;
    };

    constexpr SBind EXPANSE_BINDS[] = {
        {HL_MODIFIER_ALT, KEY_GRAVE},
        {HL_MODIFIER_ALT | HL_MODIFIER_SHIFT, KEY_GRAVE},
        {HL_MODIFIER_META, KEY_LEFT},
        {HL_MODIFIER_META, KEY_RIGHT},
        {HL_MODIFIER_META, KEY_UP},
        {HL_MODIFIER_META, KEY_DOWN},
        {HL_MODIFIER_CTRL | HL_MODIFIER_ALT, KEY_C},
    };

    bool expanseBind(uint32_t mods, uint32_t key) {
        for (const auto& bind : EXPANSE_BINDS)
            if (bind.mods == mods && bind.key == key)
                return true;
        return false;
    }

    // The surface that had keyboard focus before the overview. sendKeyboardKey
    // and sendKeyboardMods both go to this client (SeatManager.cpp).
    struct SClient {
        uint32_t mods = 0;
        struct SKey {
            uint32_t key;
            bool     pressed;
            uint32_t mods;
        };
        std::vector<SKey> keys;
        bool              paste    = false; // Ctrl+V into the focused text field
        bool              closeTab = false; // Ctrl+W
        bool              quit     = false; // Ctrl+Q
        bool              altF4    = false;
        int               keysReceived = 0;

        void onMods(uint32_t depressed) {
            mods = depressed;
        }

        void onKey(uint32_t key, bool pressed) {
            keys.push_back({key, pressed, mods});
            ++keysReceived;
            if (!pressed)
                return;
            if ((mods & HL_MODIFIER_CTRL) && key == KEY_V)
                paste = true;
            if ((mods & HL_MODIFIER_CTRL) && key == KEY_W)
                closeTab = true;
            if ((mods & HL_MODIFIER_CTRL) && key == KEY_Q)
                quit = true;
            if ((mods & HL_MODIFIER_ALT) && key == KEY_F4)
                altF4 = true;
        }
    };

    struct SDelivery {
        bool cancelled = false;
        bool keybind   = false;
        bool client    = false;
    };

    CInputManager g_input;
    // Keys whose press was forwarded (not cancelled, not consumed). The
    // matching release is suppressed when the press was not sent, as
    // CKeybindManager::onKeyEvent does with SPressedKey::sent.
    std::vector<uint32_t> g_sent;

    void resetCanvas(CCanvas::eState state, bool diving) {
        g_canvas.m_state = state;
        g_canvas.m_diving = diving;
        g_canvas.m_swallowedKeys.clear();
        g_canvas.m_spaceHeld = false;
        g_canvas.m_coast.active = false;
        g_input.m_mods = 0;
        g_sent.clear();
    }

    // main.cpp CANCEL, then InputManager::onKeyboardKey for a physical
    // keyboard that is not an input-method grab and is not captured.
    SDelivery deliverKey(uint32_t keycode, bool pressed, SClient& client) {
        SDelivery out;
        // Plugin listener. Cancelled events return before keybinds and
        // before sendKeyboardKey.
        const bool SWALLOW = g_canvas.onKey(keycode, pressed);
        if (SWALLOW) {
            out.cancelled = true;
            return out;
        }

        bool passEvent = true;
        if (pressed) {
            if (expanseBind(g_input.m_mods, keycode)) {
                passEvent = false;
                g_sent.erase(std::remove(g_sent.begin(), g_sent.end(), keycode), g_sent.end());
            } else
                g_sent.push_back(keycode);
        } else {
            const auto IT = std::find(g_sent.begin(), g_sent.end(), keycode);
            if (IT == g_sent.end())
                passEvent = false;
            else
                g_sent.erase(IT);
        }

        if (!passEvent) {
            out.keybind = pressed && expanseBind(g_input.m_mods, keycode);
            return out;
        }

        // SeatManager::sendKeyboardKey to the focused client.
        client.onKey(keycode, pressed);
        out.client = true;
        return out;
    }

    // Keyboard.cpp emits the key first, then updateXkbStateWithKey, which
    // emits modifiers. onKeyboardMod is not cancellable and always calls
    // sendKeyboardMods for the focused client.
    void modifierDown(uint32_t key, uint32_t bit, SClient& client) {
        deliverKey(key, true, client);
        g_input.m_mods |= bit;
        client.onMods(g_input.m_mods);
    }

    void modifierUp(uint32_t key, uint32_t bit, SClient& client) {
        deliverKey(key, false, client);
        g_input.m_mods &= ~bit;
        client.onMods(g_input.m_mods);
    }

    struct SChordResult {
        SClient client;
        bool    keyReachedClient = false;
        bool    keybind          = false;
        bool    keyCancelled     = false;
    };

    SChordResult chord(CCanvas::eState state, bool diving, uint32_t modKey, uint32_t modBit, uint32_t key, uint32_t extraMods = 0) {
        resetCanvas(state, diving);
        SChordResult result;
        if (extraMods) {
            g_input.m_mods |= extraMods;
            result.client.onMods(g_input.m_mods);
        }
        if (modBit)
            modifierDown(modKey, modBit, result.client);
        const SDelivery PRESS = deliverKey(key, true, result.client);
        result.keyReachedClient = PRESS.client;
        result.keybind          = PRESS.keybind;
        result.keyCancelled     = PRESS.cancelled;
        deliverKey(key, false, result.client);
        if (modBit)
            modifierUp(modKey, modBit, result.client);
        return result;
    }

    void expectSwallowed(const char* name, CCanvas::eState state, bool diving, uint32_t modKey, uint32_t modBit, uint32_t key, uint32_t extra = 0) {
        const auto GOT = chord(state, diving, modKey, modBit, key, extra);
        const bool OK = GOT.keyCancelled && !GOT.keyReachedClient && !GOT.client.paste && !GOT.client.closeTab && !GOT.client.quit && !GOT.client.altF4 &&
                       GOT.client.keysReceived == 0;
        if (OK)
            return;
        ++g_failures;
        std::printf("FAIL %s (cancelled=%d clientKey=%d paste=%d closeTab=%d quit=%d altF4=%d keys=%d)\n", name, GOT.keyCancelled, GOT.keyReachedClient,
                    GOT.client.paste, GOT.client.closeTab, GOT.client.quit, GOT.client.altF4, GOT.client.keysReceived);
    }

    void expectClient(const char* name, CCanvas::eState state, bool diving, uint32_t modKey, uint32_t modBit, uint32_t key) {
        const auto GOT = chord(state, diving, modKey, modBit, key);
        expect(GOT.keyReachedClient && !GOT.keyCancelled, name);
    }

    void expectBind(const char* name, uint32_t modKey, uint32_t modBit, uint32_t key, uint32_t extra = 0) {
        const auto GOT = chord(CCanvas::STATE_OVERVIEW, false, modKey, modBit, key, extra);
        // The bound key reaches Hyprland's dispatcher and not the client.
        // Modifier keycodes that leaked on the way are a separate case.
        expect(GOT.keybind && !GOT.keyCancelled && !GOT.keyReachedClient, name);
    }

} // namespace

int main() {
    g_pInputManager = &g_input;

    // The bug: zoomed out, modifier chords reach the window that kept focus.
    expectSwallowed("overview swallows Ctrl+V so the focused client cannot paste", CCanvas::STATE_OVERVIEW, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectSwallowed("overview swallows right-Ctrl+V", CCanvas::STATE_OVERVIEW, false, KEY_RIGHTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectSwallowed("overview swallows Ctrl+Shift+V", CCanvas::STATE_OVERVIEW, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V, HL_MODIFIER_SHIFT);
    expectSwallowed("overview swallows Ctrl+W so a tab is not closed", CCanvas::STATE_OVERVIEW, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_W);
    expectSwallowed("overview swallows Ctrl+Q so the app is not quit", CCanvas::STATE_OVERVIEW, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_Q);
    expectSwallowed("overview swallows Alt+F4", CCanvas::STATE_OVERVIEW, false, KEY_LEFTALT, HL_MODIFIER_ALT, KEY_F4);
    expectSwallowed("overview swallows right-Alt+F4", CCanvas::STATE_OVERVIEW, false, KEY_RIGHTALT, HL_MODIFIER_ALT, KEY_F4);
    expectSwallowed("overview swallows Alt+Tab", CCanvas::STATE_OVERVIEW, false, KEY_LEFTALT, HL_MODIFIER_ALT, KEY_TAB);
    expectSwallowed("overview swallows Super+Q", CCanvas::STATE_OVERVIEW, false, KEY_LEFTMETA, HL_MODIFIER_META, KEY_Q);
    expectSwallowed("overview swallows right-Super+C", CCanvas::STATE_OVERVIEW, false, KEY_RIGHTMETA, HL_MODIFIER_META, KEY_C);
    expectSwallowed("overview swallows Ctrl+Alt+T", CCanvas::STATE_OVERVIEW, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_T, HL_MODIFIER_ALT);
    expectSwallowed("opening swallows Ctrl+V", CCanvas::STATE_OPENING, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectSwallowed("diving swallows Ctrl+V", CCanvas::STATE_GLIDING, true, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectSwallowed("diving swallows Ctrl+W", CCanvas::STATE_GLIDING, true, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_W);

    // Already swallowed. A fix has to keep these swallowed.
    expectSwallowed("overview swallows a plain V", CCanvas::STATE_OVERVIEW, false, 0, 0, KEY_V);
    expectSwallowed("overview swallows Shift+V", CCanvas::STATE_OVERVIEW, false, KEY_LEFTSHIFT, HL_MODIFIER_SHIFT, KEY_V);
    expectSwallowed("overview swallows Super+A", CCanvas::STATE_OVERVIEW, false, KEY_LEFTMETA, HL_MODIFIER_META, KEY_A);
    expectSwallowed("overview swallows Super+W", CCanvas::STATE_OVERVIEW, false, KEY_LEFTMETA, HL_MODIFIER_META, KEY_W);

    // Not the canvas's keyboard. These must keep reaching the focused client.
    expectClient("idle delivers Ctrl+V", CCanvas::STATE_IDLE, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectClient("a glide that is not a dive delivers Ctrl+V", CCanvas::STATE_GLIDING, false, KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_V);
    expectClient("idle delivers a plain V", CCanvas::STATE_IDLE, false, 0, 0, KEY_V);

    // Binds the canvas leaves for Hyprland. They must not be cancelled, and
    // the focused client still must not see them.
    expectBind("overview leaves Alt+` for the overview bind", KEY_LEFTALT, HL_MODIFIER_ALT, KEY_GRAVE);
    expectBind("overview leaves Alt+Shift+` for the auto-centre bind", KEY_LEFTALT, HL_MODIFIER_ALT, KEY_GRAVE, HL_MODIFIER_SHIFT);
    expectBind("overview leaves Super+Left for the hop bind", KEY_LEFTMETA, HL_MODIFIER_META, KEY_LEFT);
    expectBind("overview leaves Super+Right for the hop bind", KEY_LEFTMETA, HL_MODIFIER_META, KEY_RIGHT);
    expectBind("overview leaves Super+Up for the hop bind", KEY_LEFTMETA, HL_MODIFIER_META, KEY_UP);
    expectBind("overview leaves Super+Down for the hop bind", KEY_LEFTMETA, HL_MODIFIER_META, KEY_DOWN);
    expectBind("overview leaves Ctrl+Alt+C for the centre bind", KEY_LEFTCTRL, HL_MODIFIER_CTRL, KEY_C, HL_MODIFIER_ALT);

    // The Ctrl key of that chord is not itself a bind. With Alt already
    // down it is a modifier chord, and today it is delivered to the client.
    {
        resetCanvas(CCanvas::STATE_OVERVIEW, false);
        SClient client;
        g_input.m_mods = HL_MODIFIER_ALT;
        client.onMods(g_input.m_mods);
        const SDelivery PRESS = deliverKey(KEY_LEFTCTRL, true, client);
        expect(PRESS.cancelled && !PRESS.client && client.keysReceived == 0, "overview swallows Ctrl while Alt is already held");
    }

    // Ctrl+V is a press and a release. Swallowing only the press still
    // delivers the release to the focused client.
    {
        resetCanvas(CCanvas::STATE_OVERVIEW, false);
        SClient client;
        modifierDown(KEY_LEFTCTRL, HL_MODIFIER_CTRL, client);
        deliverKey(KEY_V, true, client);
        const SDelivery RELEASE = deliverKey(KEY_V, false, client);
        expect(RELEASE.cancelled && !RELEASE.client && client.keysReceived == 0, "overview swallows the Ctrl+V release as well as the press");
    }

    if (g_failures == 0)
        std::printf("key swallow: all cases pass\n");
    else
        std::printf("key swallow: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
