#pragma once

#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprutils/math/Box.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace Expanse {

    // The camera maps the canvas to one monitor. `origin` is the canvas point
    // at the monitor's top-left corner, in logical pixels; `scale` is the zoom.
    // Canvas coordinates are the windows' real (global logical) positions, so
    // the home camera (origin = monitor position, scale 1) shows every window
    // exactly where Hyprland would draw it.
    struct SCamera {
        Vector2D origin;
        double   scale = 1.0;
    };

    class CCanvas {
      public:
        enum eState : uint8_t {
            STATE_IDLE,     // 100%, Hyprland draws the windows where they are
            STATE_OPENING,  // camera moving from home to fit-all
            STATE_OVERVIEW, // zoomed out, waiting for a pick
            STATE_GLIDING,  // camera moving to a window (or back home)
        };

        void                     toggle();
        void                     open();
        void                     close();
        void                     glideTo(PHLWINDOW window);
        bool                     pickAt(const Vector2D& pos); // a click at a global logical point

        void                     onPreRender(PHLMONITOR monitor);
        bool                     onMouseMove(const Vector2D& pos);
        bool                     onMouseButton(uint32_t button, bool pressed);
        bool                     onKey(uint32_t keycode, bool pressed);

        bool                     live() const;
        bool                     isCanvasWindow(PHLWINDOW window) const;
        bool                     drawsOn(PHLMONITOR monitor) const;
        PHLWINDOW                hovered() const;
        PHLWORKSPACE             workspace() const;
        double                   zoomedOut() const; // 0 at 100%, 1 once well zoomed out

        std::vector<PHLWINDOW>   canvasWindows() const; // bottom to top
        CBox                     screenBox(PHLWINDOW window) const; // global logical, through the camera
        PHLWINDOW                windowAt(const Vector2D& pos) const;

        std::string              stateJson() const;
        void                     reset();

      private:
        CBox                     usableArea() const;
        SCamera                  homeCamera() const;
        SCamera                  fitCamera() const;
        SCamera                  cameraFor(PHLWINDOW window) const;
        void                     animateTo(const SCamera& to, eState state, double seconds);
        void                     floatTiledWindows();
        void                     commit();

        PHLMONITORREF            m_monitor;
        PHLWORKSPACEREF          m_workspace;
        PHLWINDOWREF             m_hover;
        PHLWINDOWREF             m_target;

        eState                   m_state = STATE_IDLE;
        SCamera                  m_camera;
        SCamera                  m_from;
        SCamera                  m_to;
        std::chrono::steady_clock::time_point m_animStart;
        double                   m_animSeconds = 0.0;

        // Total distance the canvas has scrolled under the screen since load,
        // so a canvas position stays meaningful across glides.
        Vector2D                 m_scrolled;

        std::vector<uint32_t>    m_swallowedKeys;
    };

    inline CCanvas g_canvas;
}
