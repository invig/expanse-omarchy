#define WLR_USE_UNSTABLE

#include "HotCorner.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <wayland-server-core.h>

#include <format>

namespace Expanse {

    constexpr double TOLERANCE = 3.0;  // logical px from the very corner
    constexpr int    DWELL_MS  = 60;   // resting there this long
    constexpr int    QUIET_MS  = 1500; // after a screen change, while Hyprland moves the pointer about

    std::string CHotCorner::corner() const {
        static auto PCORNER = CConfigValue<Config::STRING>("plugin:expanse:hot_corner");
        return choice.value_or(std::string{*PCORNER});
    }

    bool CHotCorner::inCorner(const Vector2D& pos) const {
        const std::string CORNER = corner();
        if (CORNER == "off")
            return false;

        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return false;
        const CBox BOX    = {MONITOR->m_position, MONITOR->m_size};
        const bool LEFT   = pos.x <= BOX.x + TOLERANCE;
        const bool RIGHT  = pos.x >= BOX.x + BOX.w - TOLERANCE;
        const bool TOP    = pos.y <= BOX.y + TOLERANCE;
        const bool BOTTOM = pos.y >= BOX.y + BOX.h - TOLERANCE;

        if (CORNER == "all")
            return (TOP || BOTTOM) && (LEFT || RIGHT);
        if (CORNER == "top_left")
            return TOP && LEFT;
        if (CORNER == "top_right")
            return TOP && RIGHT;
        if (CORNER == "bottom_left")
            return BOTTOM && LEFT;
        if (CORNER == "bottom_right")
            return BOTTOM && RIGHT;
        return BOTTOM && (LEFT || RIGHT); // "bottom"
    }

    void CHotCorner::onMouseMove(const Vector2D& pos) {
        m_pos = pos;
        if (!inCorner(pos) || m_buttons > 0) {
            // Out of the corner (or dragging): it arms again.
            if (std::chrono::steady_clock::now() >= m_quietUntil)
                m_armed = true;
            stop();
            return;
        }
        if (!m_armed || m_dwell || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;

        m_dwell = wl_event_loop_add_timer(
            g_pCompositor->m_wlEventLoop,
            [](void* data) {
                sc<CHotCorner*>(data)->fire();
                return 0;
            },
            this);
        wl_event_source_timer_update(m_dwell, DWELL_MS);
    }

    void CHotCorner::fire() {
        stop();
        if (!m_armed || m_buttons > 0 || !inCorner(m_pos))
            return;
        // Once per visit: it fires again after the pointer leaves the corner.
        m_armed = false;
        if (onTrigger)
            onTrigger();
    }

    void CHotCorner::disarm() {
        // As on the Mac, a corner fires when you move into it: not when a
        // monitor goes away and leaves the pointer there, and the next window
        // closing (Hyprland re-sends the pointer's position) zooms out.
        stop();
        m_armed      = false;
        m_quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(QUIET_MS);
    }

    void CHotCorner::onButton(bool pressed) {
        m_buttons = std::max(0, m_buttons + (pressed ? 1 : -1));
    }

    std::string CHotCorner::describe() const {
        return std::format(R"({{"corner":"{}","chosen":{},"armed":{},"buttons":{},"waiting":{},"inCorner":{}}})", corner(), choice.has_value(), m_armed, m_buttons,
                           m_dwell != nullptr, inCorner(m_pos));
    }

    void CHotCorner::stop() {
        if (m_dwell)
            wl_event_source_remove(m_dwell);
        m_dwell = nullptr;
    }
}
