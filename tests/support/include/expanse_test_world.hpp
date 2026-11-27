#pragma once

// Stand-in for the Hyprland 0.56.2 (efb50993780079460b0cbed1363e2166a2de1d9f)
// types Expanse is built against. Methods that decide the finding's outcome
// follow the Hyprland source cited on each one. Everything else is only enough
// for Expanse's translation units to compile and run the canvas path.

#include <hyprutils/math/Box.hpp>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <typeinfo>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Hyprutils::Memory {
    template <typename To, typename From>
    constexpr To sc(From&& from) noexcept {
        return static_cast<To>(std::forward<From>(from));
    }
    template <typename To, typename From>
    constexpr To rc(From&& from) noexcept {
        return reinterpret_cast<To>(std::forward<From>(from));
    }

    template <typename T, typename... Args>
    [[nodiscard]] inline std::unique_ptr<T> makeUnique(Args&&... args) {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
    template <typename T, typename... Args>
    [[nodiscard]] inline std::shared_ptr<T> makeShared(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
}

using namespace Hyprutils::Memory;

template <typename T>
using SP = std::shared_ptr<T>;
template <typename T>
using UP = std::unique_ptr<T>;

template <typename T>
class WP {
  public:
    WP() = default;
    WP(const SP<T>& s) : w(s), raw(s.get()) {}
    WP& operator=(const SP<T>& s) {
        w   = s;
        raw = s.get();
        return *this;
    }
    WP& operator=(std::nullptr_t) {
        reset();
        return *this;
    }
    T*     operator->() const { return raw; }
    T&     operator*() const { return *raw; }
    SP<T>  lock() const { return w.lock(); }
    void   reset() {
        w.reset();
        raw = nullptr;
    }
    bool expired() const { return w.expired(); }
    explicit operator bool() const { return raw && !w.expired(); }
    bool operator==(const WP& o) const { return raw == o.raw; }
    bool operator!=(const WP& o) const { return raw != o.raw; }
    bool operator==(const SP<T>& s) const { return raw == s.get(); }
    bool operator!=(const SP<T>& s) const { return raw != s.get(); }

  private:
    std::weak_ptr<T> w;
    T*               raw = nullptr;
};

namespace Math {
    // hyprland src/helpers/math/Direction.hpp
    enum eDirection : int8_t {
        DIRECTION_DEFAULT = -1,
        DIRECTION_UP,
        DIRECTION_RIGHT,
        DIRECTION_DOWN,
        DIRECTION_LEFT,
    };
}

namespace Config {
    using INTEGER = int64_t;
    using BOOL    = bool;
    using STRING  = std::string;
    namespace Actions {
        inline bool moveFocus(Math::eDirection) { return false; }
        template <typename T>
        inline bool changeWorkspace(const T&) { return false; }
    }
}

template <typename T>
class CConfigValue {
  public:
    explicit CConfigValue(const std::string& name) : m_name(name) {}
    T operator*() const {
        if (m_name == "plugin:expanse:hot_corner")
            return T{};
        return T{};
    }

  private:
    std::string m_name;
};

template <>
inline Config::INTEGER CConfigValue<Config::INTEGER>::operator*() const {
    // general:modal_parent_blocking defaults to true. Hyprland ConfigValues.cpp:193.
    if (m_name == "general:modal_parent_blocking")
        return 1;
    if (m_name == "input:follow_mouse")
        return 1;
    return 0;
}

template <>
inline Config::BOOL CConfigValue<Config::BOOL>::operator*() const {
    if (m_name == "plugin:expanse:wallpaper")
        return true;
    return false;
}

template <>
inline std::string CConfigValue<std::string>::operator*() const {
    return "bottom";
}

namespace Log {
    enum eLogLevel { DEBUG, INFO, ERR, CRIT, TRACE };
    struct CLogger {
        template <typename... Args>
        void log(eLogLevel, std::format_string<Args...>, Args&&...) {}
    };
    inline SP<CLogger> logger = std::make_shared<CLogger>();
}

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

class IKeyboard {
  public:
    virtual ~IKeyboard() = default;
    struct SKeyEvent {
        uint32_t              timeMs  = 0;
        uint32_t              keycode = 0;
        wl_keyboard_key_state state   = WL_KEYBOARD_KEY_STATE_PRESSED;
    };
};

class IPointer {
  public:
    struct SAxisEvent {
        wl_pointer_axis_source source = WL_POINTER_AXIS_SOURCE_WHEEL;
        wl_pointer_axis        axis   = WL_POINTER_AXIS_VERTICAL_SCROLL;
        double                 delta  = 0;
    };
    struct SButtonEvent {};
    struct SPinchBeginEvent {};
    struct SPinchUpdateEvent {
        double scale = 1;
    };
    struct SPinchEndEvent {};
};

struct CClient {
    int                   id = 0;
    std::vector<uint32_t> pressedKeys;
};

struct CWLSurfaceResource {
    SP<CClient> owner;
    // SeatManager.cpp uses surf->client().
    SP<CClient> client() const { return owner; }
};

struct CSeatKeyboard {
    SP<CClient> owner;
    // SeatManager.cpp:266 k->sendKey(timeMs, key, state_)
    void sendKey(uint32_t, uint32_t key, wl_keyboard_key_state state) {
        if (owner && state == WL_KEYBOARD_KEY_STATE_PRESSED)
            owner->pressedKeys.push_back(key);
    }
    void sendEnter(SP<CWLSurfaceResource>, wl_array*) {}
    void sendLeave() {}
    void sendMods(uint32_t, uint32_t, uint32_t, uint32_t) {}
};

struct CWLSeatResource {
    SP<CClient>                    owner;
    std::vector<SP<CSeatKeyboard>> m_keyboards;
    SP<CClient>                    client() const { return owner; }
};

struct CSeatState {
    WP<CWLSurfaceResource> keyboardFocus;
    WP<CWLSeatResource>    keyboardFocusResource;
    WP<CWLSurfaceResource> pointerFocus;
};

class CSeatManager {
  public:
    CSeatState                          m_state;
    SP<IKeyboard>                       m_keyboard;
    std::vector<SP<CWLSeatResource>>    m_seatResources;

    // SeatManager.cpp:182. Without a keyboard it refuses to move keyboard focus.
    void setKeyboardFocus(SP<CWLSurfaceResource> surf) {
        if (m_state.keyboardFocus.lock() == surf)
            return;
        if (!m_keyboard)
            return;
        m_state.keyboardFocusResource.reset();
        m_state.keyboardFocus = surf;
        if (!surf)
            return;
        auto client = surf->client();
        for (auto const& r : m_seatResources) {
            if (!r || r->client() != client)
                continue;
            m_state.keyboardFocusResource = r;
            break;
        }
    }

    // SeatManager.cpp:255. Keys go only to the client that owns the keyboard focus.
    void sendKeyboardKey(uint32_t timeMs, uint32_t key, wl_keyboard_key_state state) {
        if (!m_state.keyboardFocusResource.lock())
            return;
        const auto FOCUS_CLIENT = m_state.keyboardFocusResource->client();
        for (auto const& s : m_seatResources) {
            if (!s || s->client() != FOCUS_CLIENT)
                continue;
            for (auto const& k : s->m_keyboards) {
                if (!k)
                    continue;
                k->sendKey(timeMs, key, state);
            }
        }
    }
};

inline UP<CSeatManager> g_pSeatManager = std::make_unique<CSeatManager>();

namespace Desktop::View {
    class IGeometric {
      public:
        enum eGeometricValueType : uint8_t { GEOMETRIC_CURRENT = 0, GEOMETRIC_GOAL };
    };
    enum eViewType : uint8_t { VIEW_TYPE_WINDOW = 0, VIEW_TYPE_LAYER_SURFACE };
    class IView {
      public:
        virtual ~IView() = default;
        virtual eViewType type() const { return VIEW_TYPE_WINDOW; }
    };
}

class CWorkspace;
class CMonitor;
class CWindow;
namespace Layout {
    class ITarget;
    class CSpace;
    class CAlgorithm;
    class ITiledAlgorithm;
    enum eRectCorner : uint8_t { CORNER_NONE = 0 };
    enum eRecalculateReason : uint8_t { RECALCULATE_REASON_UNKNOWN = 0 };
}

using PHLWORKSPACE    = SP<CWorkspace>;
using PHLWORKSPACEREF = WP<CWorkspace>;
using PHLWINDOW       = SP<CWindow>;
using PHLWINDOWREF    = WP<CWindow>;
using PHLMONITOR      = SP<CMonitor>;
using PHLMONITORREF   = WP<CMonitor>;

namespace Desktop::View {
    bool validMapped(const PHLWINDOW& w);
    bool validMapped(const PHLWINDOWREF& w);
}

struct CXDGDialog {
    bool modal = false;
};

struct CXDGToplevelResource {
    struct {
        std::string appid;
        std::string title;
    } m_state;
    SP<CXDGToplevelResource>              m_parent;
    PHLWINDOWREF                          m_window;
    SP<CXDGDialog>                        m_dialog;
    std::vector<WP<CXDGToplevelResource>> m_children;
    // XDGShell.cpp:350
    bool anyChildModal() const {
        return std::ranges::any_of(m_children, [](const auto& child) { return child && child->m_dialog && child->m_dialog->modal; });
    }
};

struct CXDGSurfaceResource {
    SP<CXDGToplevelResource> m_toplevel;
};

struct CXWaylandSurface {
    bool                 m_overrideRedirect = false;
    bool                 m_modal            = false;
    bool                 m_wantsFocus       = false;
    SP<CXWaylandSurface> m_parent;
    struct {
        std::string appid;
        std::string title;
    } m_state;
    bool wantsFocus() const { return m_wantsFocus; }
};

struct CTagKeeper {
    bool isTagged(const std::string&) const { return false; }
};
struct CWindowRuleApplicator {
    CTagKeeper m_tagKeeper;
};

struct CGroup {
    size_t size() const { return 1; }
    void   setCurrent(PHLWINDOW) {}
};

namespace Desktop {
    // FocusState.hpp
    enum eFocusReason : uint8_t {
        FOCUS_REASON_UNKNOWN = 0,
        FOCUS_REASON_FFM,
        FOCUS_REASON_KEYBIND,
        FOCUS_REASON_CLICK,
        FOCUS_REASON_NEW_WINDOW,
    };
}

namespace Fullscreen {
    enum eFullscreenMode : int8_t { FSMODE_NONE = 0, FSMODE_MAXIMIZED, FSMODE_FULLSCREEN };
    struct SFullscreenMode {
        eFullscreenMode internal = FSMODE_NONE;
        eFullscreenMode client   = FSMODE_NONE;
    };
    class CController {
      public:
        bool            isFullscreen(const PHLWINDOW&) const { return false; }
        PHLWINDOW       getFullscreenWindow(const PHLWORKSPACE&) const { return nullptr; }
        SFullscreenMode getFullscreenModes(const PHLWINDOW&) const { return {}; }
        bool            layoutManagedFS(const PHLWINDOW&) const { return false; }
        void            setFullscreenMode(const PHLWINDOW&, eFullscreenMode, eFullscreenMode, bool) {}
    };
    inline CController* controller() {
        static CController instance;
        return &instance;
    }
}

class CMonitor {
  public:
    Vector2D    m_position;
    Vector2D    m_size;
    std::string m_name;
    bool        m_enabled = true;
    PHLWORKSPACE m_activeWorkspace;
    CBox        logicalBoxMinusReserved() const { return {m_position, m_size}; }
};

class CWorkspace {
  public:
    std::string        m_name;
    int64_t            m_id                 = 1;
    bool               m_isSpecialWorkspace = false;
    bool               m_visible            = true;
    WP<CMonitor>       m_monitor;
    SP<Layout::CSpace> m_space;
    bool               isVisible() const { return m_visible; }
    bool               inert() const { return false; }
    struct CAnim {
        void warp() {}
    };
    SP<CAnim> m_renderOffset = std::make_shared<CAnim>();
    SP<CAnim> m_alpha        = std::make_shared<CAnim>();
};

namespace Layout {
    struct SGeometryRequested {
        Vector2D                size;
        std::optional<Vector2D> pos;
    };
    enum eGeometryFailure : uint8_t { GEOMETRY_NO_DESIRED = 0 };

    class ITarget {
      public:
        virtual ~ITarget() = default;
        virtual bool                                                floating() const        = 0;
        virtual void                                                setFloating(bool)       = 0;
        virtual CBox                                                position() const        = 0;
        virtual void                                                setPositionGlobal(const CBox&) = 0;
        virtual void                                                warpPositionSize()      = 0;
        virtual PHLWINDOW                                           window() const          = 0;
        virtual SP<CSpace>                                          space() const           = 0;
        virtual std::expected<SGeometryRequested, eGeometryFailure> desiredGeometry() const = 0;
    };

    class CSpace {
      public:
        PHLWORKSPACEREF m_workspace;
        SP<CAlgorithm>  m_algorithm;
        CBox            m_work;
        SP<CAlgorithm>  algorithm() const { return m_algorithm; }
        PHLWORKSPACE    workspace() const { return m_workspace.lock(); }
        const CBox&     workArea(bool = false) const { return m_work; }
        void            toggleTargetFloating(const SP<ITarget>& target) {
            if (target)
                target->setFloating(!target->floating());
        }
        void setTargetGeom(const CBox& box, const SP<ITarget>& target) {
            if (target)
                target->setPositionGlobal(box);
        }
    };

    class IModeAlgorithm {
      public:
        virtual ~IModeAlgorithm() = default;
        virtual void                          newTarget(SP<ITarget>)                                                       = 0;
        virtual void                          movedTarget(SP<ITarget>, std::optional<Vector2D> = std::nullopt)             = 0;
        virtual void                          removeTarget(SP<ITarget>)                                                    = 0;
        virtual void                          resizeTarget(const Vector2D&, SP<ITarget>, eRectCorner = CORNER_NONE)        = 0;
        virtual void                          recalculate(eRecalculateReason = RECALCULATE_REASON_UNKNOWN)                 = 0;
        virtual void                          swapTargets(SP<ITarget>, SP<ITarget>)                                        = 0;
        virtual void                          moveTargetInDirection(SP<ITarget>, Math::eDirection, bool)                   = 0;
        virtual std::optional<std::string>    layoutName() const { return std::nullopt; }

      protected:
        IModeAlgorithm() = default;
        WP<CAlgorithm> m_parent;
    };

    class ITiledAlgorithm : public IModeAlgorithm {
      public:
        ~ITiledAlgorithm() override = default;
        virtual SP<ITarget> getNextCandidate(SP<ITarget>) = 0;

      protected:
        ITiledAlgorithm() = default;
    };

    class CAlgorithm {
      public:
        const UP<ITiledAlgorithm>& tiledAlgo() const { return m_tiled; }
        void                       setTiled(UP<ITiledAlgorithm> tiled) { m_tiled = std::move(tiled); }
        SP<CSpace>                 space() const { return m_space.lock(); }

      private:
        UP<ITiledAlgorithm> m_tiled;
        WP<CSpace>          m_space;
    };

    class CLayoutManager {
      public:
        // LayoutManager.cpp:31. No space: it does not change floating.
        void changeFloatingMode(SP<ITarget> target) {
            if (!target || !target->space())
                return;
            target->space()->toggleTargetFloating(target);
        }
        // LayoutManager.cpp:83. A tiled target is left where it is.
        void setTargetGeom(const CBox& box, SP<ITarget> target) {
            if (!target || !target->floating())
                return;
            if (target->space())
                target->space()->setTargetGeom(box, target);
            else
                target->setPositionGlobal(box);
            target->warpPositionSize();
        }
        // LayoutManager.cpp:156
        void bringTargetToTop(SP<ITarget> target);
    };
}

inline UP<Layout::CLayoutManager> g_layoutManager = std::make_unique<Layout::CLayoutManager>();

class CWindowTarget : public Layout::ITarget {
  public:
    bool           m_floating = true;
    CBox           m_box;
    PHLWINDOWREF   m_window;
    SP<Layout::CSpace> m_space;

    bool           floating() const override { return m_floating; }
    void           setFloating(bool on) override { m_floating = on; }
    CBox           position() const override { return m_box; }
    void           setPositionGlobal(const CBox& box) override { m_box = box; }
    void           warpPositionSize() override;
    PHLWINDOW      window() const override { return m_window.lock(); }
    SP<Layout::CSpace> space() const override { return m_space; }
    std::expected<Layout::SGeometryRequested, Layout::eGeometryFailure> desiredGeometry() const override {
        return Layout::SGeometryRequested{.size = m_box.size(), .pos = m_box.pos()};
    }
};

class CWindow {
  public:
    std::string m_class;
    std::string m_initialClass;
    std::string m_title;
    std::string m_initialTitle;
    bool        m_isMapped = false;
    bool        m_isFloating = true;
    bool        m_pinned   = false;
    bool        m_hidden   = false;
    bool        m_isX11    = false;
    bool        m_noFocus  = false;
    Vector2D    m_pos;
    Vector2D    m_size{400, 300};
    Vector2D    m_relativeCursorCoordsOnLastWarp;
    PHLWORKSPACE m_workspace;
    WP<CMonitor> m_monitor;
    SP<CWindowTarget>             m_target;
    SP<CXDGSurfaceResource>       m_xdgSurface;
    SP<CXWaylandSurface>          m_xwaylandSurface;
    SP<CWLSurfaceResource>        m_surface;
    SP<CWindowRuleApplicator>     m_ruleApplicator = std::make_shared<CWindowRuleApplicator>();
    SP<CGroup>                    m_group;
    pid_t                         m_pid = 1;

    struct SurfaceHolder {
        SP<CWLSurfaceResource> res;
        SP<CWLSurfaceResource> resource() const { return res; }
    } m_wl;

    Vector2D position(Desktop::View::IGeometric::eGeometricValueType) const { return m_pos; }
    Vector2D size(Desktop::View::IGeometric::eGeometricValueType) const { return m_size; }
    CBox     layoutBox() const { return {m_pos, m_size}; }
    bool     isHidden() const { return m_hidden; }
    pid_t    getPID() const { return m_pid; }
    SurfaceHolder* wlSurface() { return &m_wl; }
    SP<Layout::ITarget> layoutTarget() const { return m_target; }
    void     warpCursor() {}
    void     sendClose() {}
    bool     acceptsInput() const { return true; }

    // Window.cpp:1677
    bool isX11OverrideRedirect() const { return m_xwaylandSurface && m_xwaylandSurface->m_overrideRedirect; }
    // Window.cpp:1681. Wayland xdg-dialog modal is not this flag.
    bool isModal() const { return m_xwaylandSurface && m_xwaylandSurface->m_modal; }

    // Window.cpp:597
    PHLWINDOW x11Parent() const;

    // Window.cpp:1805
    PHLWINDOW parent() {
        if (m_isX11) {
            auto t = x11Parent();
            if (!Desktop::View::validMapped(t))
                return nullptr;
            return t;
        }
        if (!m_xdgSurface || !m_xdgSurface->m_toplevel || !m_xdgSurface->m_toplevel->m_parent)
            return nullptr;
        if (!m_xdgSurface->m_toplevel->m_parent->m_window || !Desktop::View::validMapped(m_xdgSurface->m_toplevel->m_parent->m_window))
            return nullptr;
        return m_xdgSurface->m_toplevel->m_parent->m_window.lock();
    }

    // Window.cpp:1502. The class is the client's app id, or the X11 class string.
    std::string fetchClass() const {
        if (!m_isX11) {
            if (m_xdgSurface && m_xdgSurface->m_toplevel)
                return m_xdgSurface->m_toplevel->m_state.appid;
        } else {
            if (m_xwaylandSurface)
                return m_xwaylandSurface->m_state.appid;
        }
        return "";
    }

    // AsyncDialogBox.cpp:32 with an empty registry, and Window.cpp:1826.
    bool priorityFocus() const { return false; }
};

namespace Desktop::View {
    inline bool validMapped(const PHLWINDOW& w) { return w && w->m_isMapped; }
    inline bool validMapped(const PHLWINDOWREF& w) { return w && w->m_isMapped; }
}

inline void Layout::CLayoutManager::bringTargetToTop(SP<Layout::ITarget> target) {
    if (!target || !target->window())
        return;
    if (target->window()->m_group)
        target->window()->m_group->setCurrent(target->window());
}

inline void CWindowTarget::warpPositionSize() {
    if (const auto WINDOW = m_window.lock()) {
        WINDOW->m_pos  = m_box.pos();
        WINDOW->m_size = m_box.size();
    }
}

namespace Desktop {
    class CWindowState {
      public:
        std::vector<PHLWINDOW> m_windows;
        const std::vector<PHLWINDOW>& windows() const { return m_windows; }
        void                          raise(PHLWINDOW window) {
            std::erase(m_windows, window);
            if (window)
                m_windows.push_back(window);
        }
    };
    inline CWindowState* windowState() {
        static CWindowState state;
        return &state;
    }
}

inline PHLWINDOW CWindow::x11Parent() const {
    if (!m_isX11 || !m_xwaylandSurface || !m_xwaylandSurface->m_parent)
        return nullptr;
    for (auto const& w : Desktop::windowState()->windows()) {
        if (!w->m_isX11)
            continue;
        if (w->m_xwaylandSurface == m_xwaylandSurface->m_parent)
            return w;
    }
    return nullptr;
}

namespace Desktop {

    class CFocusState {
      public:
        PHLWINDOWREF  m_focusWindow;
        PHLMONITORREF m_focusMonitor;
        bool          m_sessionLocked = false;
        bool          m_exclusiveLayer = false;

        PHLWINDOW  window() const { return m_focusWindow.lock(); }
        PHLMONITOR monitor() const { return m_focusMonitor.lock(); }

        // FocusState.cpp:73, then rawWindowFocus at line 96, then rawSurfaceFocus at line 223.
        void fullWindowFocus(PHLWINDOW pWindow, eFocusReason) {
            if (pWindow) {
                if (!pWindow->m_workspace)
                    return;
                // Default general:modal_parent_blocking is true. A toplevel shadowed by its own
                // modal child is not focused. A normal toplevel has no modal child.
                static auto PMODALPARENTBLOCKING = CConfigValue<Config::INTEGER>("general:modal_parent_blocking");
                if (*PMODALPARENTBLOCKING && pWindow->m_xdgSurface && pWindow->m_xdgSurface->m_toplevel && pWindow->m_xdgSurface->m_toplevel->anyChildModal())
                    return;
            }
            rawWindowFocus(pWindow);
        }

        void rawWindowFocus(PHLWINDOW pWindow) {
            if (!pWindow || !pWindow->priorityFocus()) {
                if (m_sessionLocked)
                    return;
                if (m_exclusiveLayer)
                    return;
            }
            if (pWindow && pWindow->m_isX11 && pWindow->isX11OverrideRedirect() && pWindow->m_xwaylandSurface && !pWindow->m_xwaylandSurface->wantsFocus())
                return;
            if (!pWindow || !pWindow->m_isMapped) {
                m_focusWindow.reset();
                g_pSeatManager->setKeyboardFocus(nullptr);
                return;
            }
            if (pWindow->m_noFocus)
                return;
            if (!pWindow->m_workspace || !pWindow->m_workspace->isVisible())
                return;
            m_focusWindow = pWindow;
            const auto SURFACE = pWindow->wlSurface() ? pWindow->wlSurface()->resource() : nullptr;
            rawSurfaceFocus(SURFACE, pWindow);
        }

        void rawSurfaceFocus(SP<CWLSurfaceResource> pSurface, PHLWINDOW) {
            if (g_pSeatManager->m_keyboard)
                g_pSeatManager->setKeyboardFocus(pSurface);
        }
    };

    inline CFocusState* focusState() {
        static CFocusState state;
        return &state;
    }
}

namespace Desktop::View {
    class CWLSurface {
      public:
        static SP<CWLSurface> fromResource(SP<CWLSurfaceResource>) { return nullptr; }
        SP<IView>             view() const { return nullptr; }
    };
    class CLayerSurface {
      public:
        static SP<CLayerSurface> fromView(SP<IView>) { return nullptr; }
        int                      m_layer = 0;
    };
    inline PHLWINDOW CWindow_fromView(SP<IView>) { return nullptr; }
}

// CWindow::fromView is called as Desktop::View::CWindow::fromView. Provide it as a nested alias.
namespace Desktop::View {
    struct CWindow {
        static PHLWINDOW fromView(SP<IView> view) { return CWindow_fromView(view); }
    };
}

namespace State {
    class CWorkspaceStateTracker {
      public:
        std::vector<PHLWORKSPACEREF> m_workspaces;
        const std::vector<PHLWORKSPACEREF>& workspaces() const { return m_workspaces; }
    };
    class CMonitorStateTracker {
      public:
        std::vector<PHLMONITOR> m_monitors;
        const std::vector<PHLMONITOR>& monitors() const { return m_monitors; }
    };
    inline CWorkspaceStateTracker* workspaceState() {
        static CWorkspaceStateTracker state;
        return &state;
    }
    inline CMonitorStateTracker* monitorState() {
        static CMonitorStateTracker state;
        return &state;
    }
}

class CCompositor {
  public:
    wl_event_loop* m_wlEventLoop = nullptr;
};

class CHyprRenderer {
  public:
    void damageMonitor(PHLMONITOR) {}
    void damageBox(const CBox&) {}
};

class CInputManager {
  public:
    PHLWINDOWREF m_forcedFocus;
    Vector2D     getMouseCoordsInternal() { return {}; }
    void         simulateMouseMovement() {}
    uint32_t     getModsFromAllKBs() { return 0; }
};

class CEventLoopManager {
  public:
    uint64_t                                              next = 1;
    std::vector<std::pair<uint64_t, std::function<void()>>> queue;
    uint64_t doLater(std::function<void()> fn) {
        const uint64_t id = next++;
        queue.emplace_back(id, std::move(fn));
        return id;
    }
    void removeDoLater(uint64_t id) {
        std::erase_if(queue, [&](const auto& e) { return e.first == id; });
    }
};

inline UP<CCompositor>        g_pCompositor        = std::make_unique<CCompositor>();
inline UP<CHyprRenderer>      g_pHyprRenderer      = std::make_unique<CHyprRenderer>();
inline UP<CInputManager>      g_pInputManager      = std::make_unique<CInputManager>();
inline UP<CEventLoopManager>  g_pEventLoopManager  = std::make_unique<CEventLoopManager>();

namespace Pointer::Cursor {
    enum { CURSOR_OVERRIDE_SPECIAL_ACTION = 1 };
    struct COverrideController {
        void setOverride(const std::string&, int) {}
        void unsetOverride(int) {}
    };
    inline COverrideController  g_override;
    inline COverrideController* overrideController = &g_override;
}

using HANDLE = void*;

namespace HyprlandAPI {
    inline std::function<UP<Layout::ITiledAlgorithm>()> g_tiledFactory;
    inline bool addTiledAlgo(HANDLE, const std::string&, const std::type_info*, std::function<UP<Layout::ITiledAlgorithm>()>&& factory) {
        g_tiledFactory = std::move(factory);
        return true;
    }
    inline bool removeAlgo(HANDLE, const std::string&) { return true; }
}

namespace Expanse::Toast {
    inline void show(const std::string&) {}
}

// Run every callback Hyprland's doLater has queued, including ones they queue.
inline void flushLaters() {
    for (int guard = 0; guard < 32 && g_pEventLoopManager && !g_pEventLoopManager->queue.empty(); ++guard) {
        auto batch = std::move(g_pEventLoopManager->queue);
        g_pEventLoopManager->queue.clear();
        for (auto& entry : batch)
            entry.second();
    }
}
