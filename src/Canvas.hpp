#pragma once

#include "Arrange.hpp"
#include "Nearest.hpp"
#include "Store.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprutils/math/Box.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_event_source;

namespace Desktop {
    enum eFocusReason : uint8_t;
}

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

    constexpr double OVERVIEW_MAX_SCALE = 0.6; // zoomed out, windows are never drawn bigger than this

    std::optional<eDirection> parseDirection(const std::string& text);
    std::string               appName(const PHLWINDOW& window); // "org.gnome.Nautilus" → "Nautilus"

    // The bottom-right overview on the canvas: every window as a block, the
    // view as an outline, the zoom. Screen boxes are global logical.
    struct SMinimap {
        CBox              box;      // the panel
        CBox              area;     // where the map is drawn inside it
        CBox              bounds;   // the canvas region it shows
        double            scale = 1;
        Vector2D          offset;
        std::vector<CBox> windows;  // canvas
        CBox              viewport; // canvas

        CBox              toScreen(const CBox& canvas) const;
        Vector2D          toCanvas(const Vector2D& screen) const;
    };

    // Beside the minimap, as Expanse's Spaces bar: a tile per workspace on
    // the canvas's monitor, each a small map of that workspace's windows.
    // Only there with more than one workspace. Screen boxes are global logical.
    struct SSpacesBar {
        struct STile {
            PHLWORKSPACEREF   workspace;
            std::string       name;
            CBox              box;     // the tile
            CBox              screen;  // the monitor, in the tile
            std::vector<CBox> windows; // in the tile
            bool              current = false;
            bool              hovered = false;
        };
        CBox                  box; // the panel
        std::vector<STile>    tiles;

        std::optional<size_t> tileAt(const Vector2D& pos) const;
    };

    class CCanvas {
      public:
        enum eState : uint8_t {
            STATE_IDLE,     // 100%, Hyprland draws the windows where they are
            STATE_OPENING,  // camera moving from home to the overview
            STATE_OVERVIEW, // the canvas is up: pan, zoom, arrange, pick
            STATE_GLIDING,  // camera moving to a window (or back), then the real windows move
        };

        // Modes
        void                   toggle();
        void                   open();         // zoomed out to every window
        void                   openAt100();    // the canvas at 100%, to move around from here
        void                   close();        // back where you were, or where you've moved to
        void                   glideTo(PHLWINDOW window, bool fromKeyboard = false);
        void                   hop(eDirection direction);
        bool                   pickAt(const Vector2D& pos); // a click at a global logical point
        bool                   switchTo(PHLWORKSPACE workspace); // the canvas comes along, zoomed out

        // Camera, on the canvas
        void                   pan(const Vector2D& screenDelta); // moves the content by this much
        void                   zoomBy(double factor, const Vector2D& anchor);
        void                   zoomTo(double scale);             // animated, around the view's centre
        void                   fit();                            // animated, every window in view
        void                   centreOn(const Vector2D& canvasPoint);

        // Arranging, on the canvas
        void                   tidy();
        void                   selectAll();
        void                   closeWindows(const std::vector<PHLWINDOW>& windows);
        void                   setAutoCentre(bool on);
        bool                   autoCentre() const;
        void                   setWallpaper(std::optional<bool> on); // nullopt: the config value
        bool                   wallpaper() const;
        void                   setHotCorner(std::optional<std::string> corner);
        void                   restoreOffscreen(); // every window back in view

        // Remembering the canvas (CanvasStore.cpp)
        void                   restoreCanvas(); // from the saved layout: at load, and on resume
        void                   pause();         // windows back where they were before the canvas
        void                   resume();
        bool                   paused() const;
        void                   shutdown();      // saves, then puts windows back
        void                   scheduleSave();
        void                   saveNow();

        // Input; true swallows the event
        void                   onPreRender(PHLMONITOR monitor);
        bool                   onMouseMove(const Vector2D& pos);
        bool                   onMouseButton(uint32_t button, bool pressed);
        bool                   onAxis(const IPointer::SAxisEvent& event);
        bool                   onPinchBegin();
        bool                   onPinchUpdate(double scale);
        bool                   onPinchEnd();
        bool                   onKey(uint32_t keycode, bool pressed);
        void                   onFocus(PHLWINDOW window, Desktop::eFocusReason reason);
        void                   onWindowOpen(PHLWINDOW window);
        void                   onWindowClose(PHLWINDOW window);
        void                   onScreenChanged();
        void                   onWorkspaceChanged(PHLWORKSPACE workspace);
        void                   simulateScreen(std::optional<Vector2D> size); // debugging: pretend the screen is this size

        // What's drawn
        bool                   live() const;
        bool                   showsCanvas() const; // the canvas is up, not gliding
        bool                   isCanvasWindow(PHLWINDOW window) const;
        bool                   drawsOn(PHLMONITOR monitor) const;
        PHLWINDOW              hovered() const;
        PHLWORKSPACE           workspace() const;
        double                 zoomedOut() const; // 0 at 100%, 1 once well zoomed out
        double                 scale() const;
        bool                   isSelected(PHLWINDOW window) const;
        bool                   showsClose(PHLWINDOW window) const;
        std::optional<CBox>    selectionBounds() const; // canvas, with more than one selected
        std::optional<CBox>    marquee() const;         // global logical
        const std::vector<Arrange::SGuide>& guides() const; // canvas
        std::optional<SMinimap> minimap() const;              // while the canvas is up
        std::optional<SSpacesBar> spacesBar() const;          // while the canvas is up

        std::vector<PHLWINDOW> canvasWindows() const; // bottom to top
        CBox                   screenBox(PHLWINDOW window) const; // global logical, through the camera
        Vector2D               toScreen(const Vector2D& canvasPoint) const;
        Vector2D               toCanvas(const Vector2D& screenPoint) const;
        CBox                   viewInCanvas() const;
        PHLWINDOW              windowAt(const Vector2D& pos) const;
        static CBox            closeButton(const CBox& screen); // where × sits on a window drawn at `screen`

        std::string            stateJson() const;
        void                   reset();

      private:
        // Each window the canvas has touched: where it was before (to put
        // back), the size you chose, and its spot in the home layout.
        struct STracked {
            PHLWINDOWREF            window;
            bool                    wasTiled = false;
            CBox                    box;
            Vector2D                preferred;
            Vector2D                home;      // absolute canvas
            std::optional<Vector2D> lastSized; // what the canvas last resized it to
            PHLWORKSPACEREF         workspace; // whose canvas `home` is on
        };

        PHLMONITOR             monitor() const;
        bool                   begin(PHLMONITOR monitor = nullptr); // on the focused monitor, or this one
        bool                   offItsScreen(const PHLWINDOW& window); // on its monitor's canvas, not fully on that screen
        CBox                   usableArea() const;
        SCamera                homeCamera() const;
        SCamera                fitCamera(double maxScale) const;
        SCamera                cameraFor(PHLWINDOW window) const;
        SCamera                centredOn(const Vector2D& point, double scale) const;
        SCamera                zoomedAround(const SCamera& camera, double scale, const Vector2D& anchor) const;
        CBox                   viewCentre() const;
        bool                   fullyVisible(PHLWINDOW window) const;
        PHLWINDOW              nearest(eDirection direction, const CBox& from, PHLWINDOW exclude) const;
        void                   glideLater(PHLWINDOW window, bool fromKeyboard);
        void                   animateTo(const SCamera& to, eState state, double seconds);
        void                   advance(bool late); // the camera, one frame on (or to the end, late)
        void                   setCamera(const SCamera& camera);
        void                   cameraMovedByUser();
        void                   damage() const;
        void                   floatTiledWindows();
        void                   leaveFullscreen();
        void                   restoreFullscreen();
        void                   moveWindow(PHLWINDOW window, const Vector2D& canvasPos);
        void                   raise(PHLWINDOW window);
        void                   placeNewWindow(PHLWINDOW window, PHLWINDOW anchor);
        Vector2D               scrolledFor(const PHLWORKSPACE& workspace) const;
        void                   noteOriginal(PHLWINDOW window);
        void                   followWorkspaces();
        bool                   placeFromSaved(PHLWINDOW window, bool exactOnly);
        void                   putWindowsBack();
        void                   stopSaving();

        // Screens (CanvasScreen.cpp)
        STracked*              tracked(const PHLWINDOW& window);
        bool                   isDerived(const PHLWORKSPACE& workspace) const;
        void                   noteSizes();
        void                   updateHome();
        void                   noteUserMove(const PHLWINDOW& window, const Vector2D& delta);
        void                   resizeWindow(const PHLWINDOW& window, const Vector2D& size);
        void                   fitToScreen(const PHLWINDOW& window);
        void                   fitLayoutToScreen(double margin, bool tidy);
        void                   relayout();       // the focused monitor's workspace (or m_layoutMonitor's)
        void                   relayoutAll();    // every monitor's
        void                   relayoutOn(const PHLMONITOR& monitor);
        Vector2D               cameWith(const PHLWORKSPACE& workspace, const Vector2D& scroll, const Vector2D& moved) const;
        PHLWINDOW              topWindowInView(PHLWINDOW excluding) const;
        void                   settle(const SCamera& camera);
        void                   commit();
        void                   followWorkspace(PHLWORKSPACE workspace);
        std::vector<PHLWINDOW> selection() const;
        void                   clearSelection();
        void                   toggleSelected(PHLWINDOW window);

        // Input helpers (CanvasInput.cpp)
        void                   dragMove(const Vector2D& pos);
        bool                   handleCanvasKey(uint32_t keycode, uint32_t mods);

        PHLMONITORREF          m_monitor;
        PHLWORKSPACEREF        m_workspace;
        PHLWINDOWREF           m_hover;
        PHLWINDOWREF           m_target;
        PHLWORKSPACEREF        m_hoverSpace;    // the workspaces bar tile under the pointer
        PHLWINDOWREF           m_offscreenHint; // the last window we said was off-screen

        eState                 m_state        = STATE_IDLE;
        bool                   m_animating    = false;
        bool                   m_fromKeyboard = false; // the glide lands like Hyprland's movefocus
        bool                   m_autoCentre   = true;
        std::optional<bool>    m_wallpaper;
        SCamera                m_camera;
        SCamera                m_from;
        SCamera                m_to;
        std::chrono::steady_clock::time_point m_animStart;
        double                 m_animSeconds = 0.0;

        // The 100% view you left when zooming out; leaving the canvas returns
        // there unless you've panned or zoomed since.
        std::optional<SCamera> m_returnCamera;

        // How far each workspace's canvas has scrolled under the screen, so a
        // canvas position (real + scrolled) stays meaningful across glides.
        std::unordered_map<std::string, Vector2D> m_scrolled;

        std::vector<STracked>       m_originals;
        // Workspaces as last seen, to follow renames: names key the scroll
        // and screens kept per workspace.
        std::vector<std::pair<PHLWORKSPACEREF, std::string>> m_workspaceNames;
        wl_event_source*            m_relayoutTimer  = nullptr;
        wl_event_source*            m_animationTimer = nullptr;
        // Per workspace, the screen its layout was last fitted to: monitors
        // can differ, and a workspace can move between them.
        std::unordered_map<std::string, CBox> m_lastScreens;
        PHLMONITORREF               m_layoutMonitor;   // fitting a monitor other than the focused one
        PHLWORKSPACEREF             m_layoutWorkspace; // placing on a workspace that isn't showing

        // While it lives, and the canvas isn't up, canvas queries are about
        // this workspace, on its own monitor, as if it were showing there.
        struct SLookAt {
            SLookAt(CCanvas& canvas, const PHLWORKSPACE& workspace) : m_canvas(canvas), m_previous(canvas.m_layoutWorkspace) {
                m_canvas.m_layoutWorkspace = workspace;
            }
            ~SLookAt() {
                m_canvas.m_layoutWorkspace = m_previous;
            }
            CCanvas&        m_canvas;
            PHLWORKSPACEREF m_previous;
        };
        std::optional<Vector2D>     m_screenOverride;
        // Until then, Hyprland is still moving windows for a monitor change.
        std::chrono::steady_clock::time_point m_settledAt;
        std::vector<Store::SWindow> m_pool; // saved spots of windows not open now
        bool                        m_paused    = false;
        wl_event_source*            m_saveTimer = nullptr;

        std::vector<PHLWINDOWREF> m_selected;

        // A full-screen window leaves full screen while the canvas is up
        // (Hyprland hides everything under it), and gets it back if you leave
        // without picking another window, as a full-screen app on the Mac.
        struct {
            PHLWINDOWREF                window;
            Fullscreen::SFullscreenMode mode;
            bool                        layoutAware = false; // which of Hyprland's handlers had it
        } m_fullscreen;

        struct SDrag {
            enum eKind : uint8_t {
                DRAG_NONE,
                DRAG_PAN,
                DRAG_WINDOW,  // one window, or the selection it's part of
                DRAG_MARQUEE, // selecting with a box
                DRAG_MINIMAP, // the view follows the pointer on the minimap
            } kind = DRAG_NONE;
            Vector2D                  start; // pointer, global logical
            Vector2D                  last;
            PHLWINDOWREF              window;
            std::vector<std::pair<PHLWINDOWREF, CBox>> starts; // canvas boxes at the press
            std::vector<PHLWINDOWREF> base;  // the selection a ⇧-marquee adds to
            bool                      moved = false;
        } m_drag;
        std::vector<Arrange::SGuide> m_guides;

        // A left click at 100%, to tell a click from a drag on release.
        struct {
            Vector2D     pos;
            PHLWINDOWREF window;
        } m_press;

        bool                   m_spaceHeld  = false;
        bool                   m_pinching   = false;
        bool                   m_altPinch   = false;
        double                 m_pinchScale = 1.0;
        std::vector<uint32_t>  m_swallowedKeys;
    };

    inline CCanvas g_canvas;
}
