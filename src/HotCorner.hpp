#pragma once

#include <hyprutils/math/Vector2D.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

struct wl_event_source;

namespace Expanse {

    // Rest the pointer in a screen corner to toggle the canvas, as on the Mac.
    // Which corners: plugin:expanse:hot_corner, one of off, all, bottom (both
    // bottom corners, the default: Omarchy's bar has its menu top left),
    // top_left, top_right, bottom_left, bottom_right.
    class CHotCorner {
      public:
        std::function<void()> onTrigger;
        // Picked from the menu (saved with the canvas); without one, the config value.
        std::optional<std::string> choice;
        std::string                corner() const;

        void                  onMouseMove(const Hyprutils::Math::Vector2D& pos);
        void                  onButton(bool pressed);
        // The screens changed: the pointer may now sit in a corner it never
        // moved into. It fires again once the pointer has left the corner.
        void                  disarm();
        void                  stop();
        std::string           describe() const; // for hyprctl expanse

      private:
        bool                        inCorner(const Hyprutils::Math::Vector2D& pos) const;
        void                        fire();

        Hyprutils::Math::Vector2D             m_pos;
        bool                                  m_armed   = false; // until the pointer is first seen out of a corner
        int                                   m_buttons = 0;
        wl_event_source*                      m_dwell   = nullptr;
        std::chrono::steady_clock::time_point m_quietUntil; // after a screen change, moves don't arm it yet
    };

    inline CHotCorner g_hotCorner;
}
