#pragma once

// Types the canvas translation unit names while compiling. The key-swallow
// test links the real CCanvas::onKey; these stand in for Hyprland only so
// that translation unit can be compiled here. The delivery path itself is
// in tests/key_swallow_test.cpp, transcribed from Hyprland 0.56.2.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <hyprutils/math/Box.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>

using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

template <typename T>
using SP = Hyprutils::Memory::CSharedPointer<T>;
template <typename T>
using WP = Hyprutils::Memory::CWeakPointer<T>;

namespace Monitor {
    class CMonitor;
}
class CWorkspace;

using HANDLE = void*;

struct wl_event_loop;
struct wl_event_source;
using wl_event_loop_timer_func_t = int (*)(void*);
inline wl_event_source* wl_event_loop_add_timer(wl_event_loop*, wl_event_loop_timer_func_t, void*) {
    return nullptr;
}
inline void wl_event_source_timer_update(wl_event_source*, int) {}

enum wl_pointer_axis_source {
    WL_POINTER_AXIS_SOURCE_WHEEL = 0,
    WL_POINTER_AXIS_SOURCE_FINGER,
    WL_POINTER_AXIS_SOURCE_CONTINUOUS,
};
enum wl_pointer_axis {
    WL_POINTER_AXIS_VERTICAL_SCROLL = 0,
    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
};
enum wl_pointer_axis_relative_direction {
    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL = 0,
};
enum wl_pointer_button_state {
    WL_POINTER_BUTTON_STATE_RELEASED = 0,
    WL_POINTER_BUTTON_STATE_PRESSED,
};
enum wl_keyboard_key_state {
    WL_KEYBOARD_KEY_STATE_RELEASED = 0,
    WL_KEYBOARD_KEY_STATE_PRESSED,
};

// hyprland 0.56.2 src/devices/IKeyboard.hpp
enum eKeyboardModifiers {
    HL_MODIFIER_SHIFT = (1 << 0),
    HL_MODIFIER_CAPS  = (1 << 1),
    HL_MODIFIER_CTRL  = (1 << 2),
    HL_MODIFIER_ALT   = (1 << 3),
    HL_MODIFIER_MOD2  = (1 << 4),
    HL_MODIFIER_MOD3  = (1 << 5),
    HL_MODIFIER_META  = (1 << 6),
    HL_MODIFIER_MOD5  = (1 << 7),
};

class CWLSurfaceResource;

namespace Desktop::View {
    enum eViewType : uint8_t {
        VIEW_TYPE_WINDOW = 0,
        VIEW_TYPE_LAYER_SURFACE,
    };

    struct IGeometric {
        enum eKind { GEOMETRIC_GOAL = 0 };
    };

    class IView {
      public:
        virtual ~IView() = default;
        eViewType type() const {
            return m_type;
        }
        eViewType m_type = VIEW_TYPE_WINDOW;
    };

    class CWindow;
    class CLayerSurface;
    class CWLSurface;

    class CWindow : public IView {
      public:
        bool                         m_isMapped = false;
        bool                         m_pinned   = false;
        WP<Monitor::CMonitor> m_monitor;
        bool                         isHidden() const {
            return false;
        }
        Vector2D position(IGeometric::eKind) const {
            return {};
        }
        Vector2D size(IGeometric::eKind) const {
            return {};
        }
        static SP<CWindow> fromView(const SP<IView>&) {
            return nullptr;
        }
    };

    class CLayerSurface : public IView {
      public:
        int m_layer = 0;
        static SP<CLayerSurface> fromView(const SP<IView>&) {
            return nullptr;
        }
    };

    class CWLSurface {
      public:
        SP<IView> view() const {
            return nullptr;
        }
        static SP<CWLSurface> fromResource(const SP<CWLSurfaceResource>&) {
            return nullptr;
        }
    };
}

namespace Monitor {
    class CMonitor {
      public:
        bool            m_enabled = false;
        Vector2D        m_position;
        Vector2D        m_size;
        WP<CWorkspace> m_activeWorkspace;
    };
}

class CWorkspace {
  public:
    bool inert() const {
        return false;
    }
};

using PHLWORKSPACE    = SP<CWorkspace>;
using PHLWORKSPACEREF = WP<CWorkspace>;
using PHLWINDOW       = SP<Desktop::View::CWindow>;
using PHLWINDOWREF    = WP<Desktop::View::CWindow>;
using PHLMONITOR      = SP<Monitor::CMonitor>;
using PHLMONITORREF   = WP<Monitor::CMonitor>;

namespace Desktop {
    enum eFocusReason : uint8_t {
        FOCUS_REASON_UNKNOWN = 0,
        FOCUS_REASON_FFM,
        FOCUS_REASON_KEYBIND,
        FOCUS_REASON_CLICK,
    };

    class CWindowState {
      public:
        const std::vector<PHLWINDOW>& windows() const {
            return m_windows;
        }
        std::vector<PHLWINDOW> m_windows;
    };
    inline CWindowState g_windowState;
    inline CWindowState* windowState() {
        return &g_windowState;
    }

    class CFocusState {
      public:
        PHLWINDOW window() const {
            return m_window;
        }
        PHLWINDOW m_window;
    };
    inline CFocusState g_focusState;
    inline CFocusState* focusState() {
        return &g_focusState;
    }
}

namespace State {
    class CMonitorState {
      public:
        const std::vector<PHLMONITOR>& monitors() const {
            return m_monitors;
        }
        std::vector<PHLMONITOR> m_monitors;
    };
    inline CMonitorState g_monitorState;
    inline CMonitorState* monitorState() {
        return &g_monitorState;
    }
}

namespace Fullscreen {
    enum eFullscreenMode : int8_t { FSMODE_NONE = 0, FSMODE_MAXIMIZED, FSMODE_FULLSCREEN };
    struct SFullscreenMode {
        eFullscreenMode internal = FSMODE_NONE;
        eFullscreenMode client   = FSMODE_NONE;
    };
    class CFullscreenController {
      public:
        bool isFullscreen(const PHLWINDOW&) const {
            return false;
        }
    };
    inline CFullscreenController g_fullscreen;
    inline CFullscreenController* controller() {
        return &g_fullscreen;
    }
}

class IPointer {
  public:
    struct SAxisEvent {
        uint32_t                           timeMs            = 0;
        wl_pointer_axis_source             source            = WL_POINTER_AXIS_SOURCE_WHEEL;
        wl_pointer_axis                    axis              = WL_POINTER_AXIS_VERTICAL_SCROLL;
        wl_pointer_axis_relative_direction relativeDirection = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL;
        double                             delta             = 0.0;
        int32_t                            deltaDiscrete     = 0;
        bool                               mouse             = false;
    };
    struct SButtonEvent {
        uint32_t                timeMs = 0;
        uint32_t                button = 0;
        wl_pointer_button_state state  = WL_POINTER_BUTTON_STATE_PRESSED;
    };
};

class CInputManager {
  public:
    uint32_t getModsFromAllKBs() {
        return m_mods;
    }
    Vector2D getMouseCoordsInternal() {
        return {};
    }
    uint32_t m_mods = 0;
};
inline CInputManager* g_pInputManager = nullptr;

class CSeatManager {
  public:
    struct {
        WP<CWLSurfaceResource> pointerFocus;
    } m_state;
};
inline CSeatManager* g_pSeatManager = nullptr;

class CCompositor {
  public:
    wl_event_loop* m_wlEventLoop = nullptr;
};
inline CCompositor* g_pCompositor = nullptr;

namespace Config::Actions {
    inline bool changeWorkspace(const PHLWORKSPACE&) {
        return false;
    }
}

namespace Pointer::Cursor {
    enum eOverride { CURSOR_OVERRIDE_SPECIAL_ACTION = 1 };
    class COverrideController {
      public:
        void unsetOverride(eOverride) {}
        void setOverride(const std::string&, eOverride) {}
    };
    inline COverrideController g_overrideController;
    inline COverrideController* overrideController = &g_overrideController;
}
