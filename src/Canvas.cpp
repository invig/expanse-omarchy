#define WLR_USE_UNSTABLE

#include "Canvas.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace Expanse {

    using Desktop::View::IGeometric;

    constexpr double OPEN_SECONDS       = 0.32;
    constexpr double GLIDE_SECONDS      = 0.38; // from zoomed out
    constexpr double HOP_SECONDS        = 0.30; // at 100%, as on the Mac
    constexpr double PAN_SECONDS        = 0.25; // zoomed out, following the highlight
    constexpr double CLOSE_SECONDS      = 0.28;
    constexpr double OVERVIEW_MARGIN    = 48.0;
    constexpr double OVERVIEW_MAX_SCALE = 0.9;
    constexpr double OVERVIEW_MIN_SCALE = 0.05;
    constexpr double CLICK_SLOP         = 6.0; // logical px a click may move before it's a drag

    static double easeInOutCubic(double t) {
        return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
    }

    // Moves the canvas point at the centre of the view in a straight line while
    // the scale changes geometrically, so a zoom feels the same at every depth.
    static SCamera mix(const SCamera& a, const SCamera& b, double t, const Vector2D& view) {
        const double   scale   = std::exp(std::lerp(std::log(a.scale), std::log(b.scale), t));
        const Vector2D centreA = a.origin + view / (2.0 * a.scale);
        const Vector2D centreB = b.origin + view / (2.0 * b.scale);
        const Vector2D centre  = centreA + (centreB - centreA) * t;
        return {centre - view / (2.0 * scale), scale};
    }

    static CBox canvasBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_CURRENT), window->size(IGeometric::GEOMETRIC_CURRENT)};
    }

    static std::string jsonEscape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (const char c : text) {
            if (c == '"' || c == '\\')
                out += '\\';
            if (sc<unsigned char>(c) < 0x20)
                continue;
            out += c;
        }
        return out;
    }

    // The window whose surface has the pointer, as the seat sees it: a layer
    // surface or popup on top means no window.
    static PHLWINDOW windowUnderPointer() {
        const auto SURFACE = g_pSeatManager->m_state.pointerFocus.lock();
        if (!SURFACE)
            return nullptr;
        const auto WLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        return WLSURFACE ? Desktop::View::CWindow::fromView(WLSURFACE->view()) : nullptr;
    }

    std::optional<eDirection> parseDirection(const std::string& text) {
        if (text == "left" || text == "l")
            return DIRECTION_LEFT;
        if (text == "right" || text == "r")
            return DIRECTION_RIGHT;
        if (text == "up" || text == "u")
            return DIRECTION_UP;
        if (text == "down" || text == "d")
            return DIRECTION_DOWN;
        return std::nullopt;
    }

    void CCanvas::toggle() {
        switch (m_state) {
            case STATE_IDLE: open(); break;
            case STATE_OPENING:
            case STATE_OVERVIEW: close(); break;
            case STATE_GLIDING: break;
        }
    }

    bool CCanvas::begin() {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return false;

        m_monitor   = MONITOR;
        m_workspace = MONITOR->m_activeWorkspace;
        m_target.reset();
        floatTiledWindows();
        m_camera = homeCamera();
        return true;
    }

    void CCanvas::open() {
        if (live() || !begin())
            return;

        m_hover = Desktop::focusState()->window();
        animateTo(fitCamera(), STATE_OPENING, OPEN_SECONDS);
    }

    void CCanvas::close() {
        if (m_state != STATE_OPENING && m_state != STATE_OVERVIEW)
            return;

        m_target.reset();
        animateTo(homeCamera(), STATE_GLIDING, CLOSE_SECONDS);
    }

    void CCanvas::glideTo(PHLWINDOW window) {
        if (!window)
            return;

        if (!live() && !begin())
            return;

        if (!isCanvasWindow(window))
            return;

        m_target = window;
        m_hover  = window;
        // On top for the whole glide, not only once it lands.
        if (const auto TARGET = window->layoutTarget())
            g_layoutManager->bringTargetToTop(TARGET);

        animateTo(cameraFor(window), STATE_GLIDING, m_camera.scale > 0.99 ? HOP_SECONDS : GLIDE_SECONDS);
    }

    void CCanvas::glideLater(PHLWINDOW window) {
        // Out of the event that asked for it: gliding floats and moves windows.
        g_pEventLoopManager->doLater([this, ref = PHLWINDOWREF{window}] {
            if (const auto WINDOW = ref.lock(); WINDOW && !live() && isCanvasWindow(WINDOW) && !fullyVisible(WINDOW))
                glideTo(WINDOW);
        });
    }

    void CCanvas::hop(eDirection direction) {
        switch (m_state) {
            case STATE_GLIDING: {
                // Mid-glide: hop on from the window it's heading to, straight away.
                const auto CURRENT = m_target.lock();
                if (const auto NEXT = CURRENT ? nearest(direction, canvasBox(CURRENT), CURRENT) : nullptr)
                    glideTo(NEXT);
                return;
            }
            case STATE_OPENING:
            case STATE_OVERVIEW: {
                // Zoomed out: the highlight moves and the view follows.
                const auto FROM = m_hover.lock();
                const bool HAS  = FROM && isCanvasWindow(FROM);
                const auto NEXT = nearest(direction, HAS ? canvasBox(FROM) : viewCentre(), HAS ? FROM : nullptr);
                if (!NEXT)
                    return;
                m_hover = NEXT;
                animateTo(centredOn(canvasBox(NEXT).middle(), m_animating ? m_to.scale : m_camera.scale), STATE_OVERVIEW, PAN_SECONDS);
                return;
            }
            case STATE_IDLE: {
                // Every hop centres its window: one that only moved focus to a
                // window already on screen looked like nothing happened.
                const auto FOCUSED = Desktop::focusState()->window();
                const bool HAS     = isCanvasWindow(FOCUSED);
                if (const auto NEXT = nearest(direction, HAS ? canvasBox(FOCUSED) : viewCentre(), HAS ? FOCUSED : nullptr))
                    glideTo(NEXT);
                return;
            }
        }
    }

    PHLWINDOW CCanvas::nearest(eDirection direction, const CBox& from, PHLWINDOW exclude) const {
        std::vector<PHLWINDOW> windows;
        std::vector<CBox>      boxes;
        for (const auto& window : canvasWindows()) {
            if (window == exclude)
                continue;
            windows.push_back(window);
            boxes.push_back(canvasBox(window));
        }

        const auto INDEX = nearestBox(direction, from, boxes);
        return INDEX ? windows[*INDEX] : nullptr;
    }

    void CCanvas::animateTo(const SCamera& to, eState state, double seconds) {
        m_from        = m_camera;
        m_to          = to;
        m_animStart   = std::chrono::steady_clock::now();
        m_animSeconds = seconds;
        m_animating   = true;
        m_state       = state;

        if (const auto MONITOR = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(MONITOR);
    }

    void CCanvas::onPreRender(PHLMONITOR monitor) {
        if (!live() || monitor != m_monitor.lock())
            return;

        if (m_animating) {
            const double ELAPSED = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_animStart).count();
            const double T       = m_animSeconds > 0.0 ? std::clamp(ELAPSED / m_animSeconds, 0.0, 1.0) : 1.0;

            m_camera = mix(m_from, m_to, easeInOutCubic(T), monitor->m_size);

            if (T >= 1.0) {
                m_camera    = m_to;
                m_animating = false;
                if (m_state == STATE_OPENING)
                    m_state = STATE_OVERVIEW;
                else if (m_state == STATE_GLIDING) {
                    // Moving windows from inside a frame is unsafe; the camera
                    // holds the final view until the move lands.
                    g_pEventLoopManager->doLater([this] {
                        if (m_state == STATE_GLIDING && !m_animating)
                            commit();
                    });
                }
            }
        }

        // Hyprland reports damage in untransformed coordinates, so while the
        // camera is in use the whole monitor is redrawn every frame.
        g_pHyprRenderer->damageMonitor(monitor);
    }

    void CCanvas::commit() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR) {
            reset();
            return;
        }

        const Vector2D DELTA = m_camera.origin - MONITOR->m_position;

        if (DELTA.size() > 0.5) {
            for (const auto& window : canvasWindows()) {
                if (Fullscreen::controller()->isFullscreen(window))
                    continue;

                const auto TARGET = window->layoutTarget();
                if (!TARGET || !TARGET->floating())
                    continue;

                CBox box = TARGET->position();
                box.translate(-DELTA);
                g_layoutManager->setTargetGeom(box, TARGET);
                TARGET->warpPositionSize();
            }
            m_scrolled = m_scrolled + DELTA;
        }

        m_camera = homeCamera();
        m_state  = STATE_IDLE;

        if (const auto TARGET = m_target.lock(); TARGET && TARGET->m_isMapped) {
            Desktop::focusState()->fullWindowFocus(TARGET, Desktop::FOCUS_REASON_CLICK);
            if (const auto LAYOUTTARGET = TARGET->layoutTarget())
                g_layoutManager->bringTargetToTop(LAYOUTTARGET);
        }

        m_target.reset();
        m_hover.reset();
        g_pHyprRenderer->damageMonitor(MONITOR);
    }

    void CCanvas::floatTiledWindows() {
        // Record every tiled box first: floating one window makes the layout
        // re-tile the rest.
        std::vector<std::pair<SP<Layout::ITarget>, CBox>> tiled;
        for (const auto& window : canvasWindows()) {
            const auto TARGET = window->layoutTarget();
            if (TARGET && !TARGET->floating() && !Fullscreen::controller()->isFullscreen(window))
                tiled.emplace_back(TARGET, TARGET->position());
        }

        for (const auto& [target, box] : tiled)
            g_layoutManager->changeFloatingMode(target);

        for (const auto& [target, box] : tiled) {
            g_layoutManager->setTargetGeom(box, target);
            target->warpPositionSize();
        }
    }

    bool CCanvas::onMouseMove(const Vector2D& pos) {
        if (!live())
            return false;

        const auto HOVER = windowAt(pos);
        if (HOVER && HOVER != m_hover.lock()) {
            m_hover = HOVER;
            if (const auto MONITOR = m_monitor.lock())
                g_pHyprRenderer->damageMonitor(MONITOR);
        }

        // Swallowed so apps don't see a pointer over their real, off-view
        // positions and focus doesn't follow the mouse.
        return true;
    }

    bool CCanvas::onMouseButton(uint32_t button, bool pressed) {
        if (!live()) {
            // At 100%, clicking a window that isn't fully on screen centres it.
            // The click itself goes to the app; a drag (selecting, moving the
            // window) doesn't count.
            if (button != BTN_LEFT)
                return false;

            const auto POS = g_pInputManager->getMouseCoordsInternal();
            if (pressed)
                m_press = {POS, windowUnderPointer()};
            else if (const auto WINDOW = m_press.window.lock(); WINDOW && POS.distance(m_press.pos) <= CLICK_SLOP && !fullyVisible(WINDOW))
                glideLater(WINDOW);
            return false;
        }

        if (!pressed || m_state == STATE_GLIDING)
            return true;

        if (button == BTN_LEFT) {
            if (const auto WINDOW = windowAt(g_pInputManager->getMouseCoordsInternal()))
                glideTo(WINDOW);
            else
                close();
        } else if (button == BTN_RIGHT)
            close();

        return true;
    }

    bool CCanvas::onKey(uint32_t keycode, bool pressed) {
        if (!pressed) {
            const auto IT = std::ranges::find(m_swallowedKeys, keycode);
            if (IT == m_swallowedKeys.end())
                return false;
            m_swallowedKeys.erase(IT);
            return true;
        }

        if (!live())
            return false;

        if (keycode == KEY_ESC)
            close();
        else if (keycode == KEY_ENTER || keycode == KEY_KPENTER) {
            const auto WINDOW = m_hover.lock();
            if (WINDOW && isCanvasWindow(WINDOW))
                glideTo(WINDOW);
            else
                close();
        } else if (g_pInputManager->getModsFromAllKBs() & (HL_MODIFIER_META | HL_MODIFIER_ALT | HL_MODIFIER_CTRL))
            return false; // chords stay keybinds: the toggle, hops

        m_swallowedKeys.push_back(keycode);
        return true;
    }

    void CCanvas::onFocus(PHLWINDOW window, Desktop::eFocusReason reason) {
        // Keyboard focus (binds, Alt-Tab, a window closing) centres a window
        // that isn't fully on screen. Clicks are handled on release; focus
        // that follows the mouse never moves the canvas.
        if (live() || reason == Desktop::FOCUS_REASON_FFM || reason == Desktop::FOCUS_REASON_CLICK)
            return;

        if (isCanvasWindow(window) && !fullyVisible(window))
            glideLater(window);
    }

    bool CCanvas::live() const {
        return m_state != STATE_IDLE;
    }

    PHLMONITOR CCanvas::monitor() const {
        return live() ? m_monitor.lock() : Desktop::focusState()->monitor();
    }

    PHLWORKSPACE CCanvas::workspace() const {
        if (live())
            return m_workspace.lock();
        const auto MONITOR = monitor();
        return MONITOR ? MONITOR->m_activeWorkspace : nullptr;
    }

    bool CCanvas::isCanvasWindow(PHLWINDOW window) const {
        if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned)
            return false;

        const auto WORKSPACE = workspace();
        return WORKSPACE && window->m_workspace == WORKSPACE;
    }

    bool CCanvas::drawsOn(PHLMONITOR monitor) const {
        return live() && monitor && monitor == m_monitor.lock();
    }

    PHLWINDOW CCanvas::hovered() const {
        return m_hover.lock();
    }

    double CCanvas::zoomedOut() const {
        return live() ? std::clamp((1.0 - m_camera.scale) / 0.25, 0.0, 1.0) : 0.0;
    }

    std::vector<PHLWINDOW> CCanvas::canvasWindows() const {
        std::vector<PHLWINDOW> windows;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (isCanvasWindow(window))
                windows.push_back(window);
        }
        return windows;
    }

    CBox CCanvas::usableArea() const {
        const auto MONITOR = monitor();
        return MONITOR ? MONITOR->logicalBoxMinusReserved() : CBox{};
    }

    bool CCanvas::fullyVisible(PHLWINDOW window) const {
        const CBox AREA = usableArea().expand(1);
        const CBox BOX  = screenBox(window);
        return BOX.x >= AREA.x && BOX.y >= AREA.y && BOX.x + BOX.w <= AREA.x + AREA.w && BOX.y + BOX.h <= AREA.y + AREA.h;
    }

    SCamera CCanvas::homeCamera() const {
        const auto MONITOR = monitor();
        return {MONITOR ? MONITOR->m_position : Vector2D{}, 1.0};
    }

    SCamera CCanvas::centredOn(const Vector2D& point, double scale) const {
        const auto MONITOR = monitor();
        if (!MONITOR)
            return homeCamera();
        return {point - (usableArea().middle() - MONITOR->m_position) / scale, scale};
    }

    CBox CCanvas::viewCentre() const {
        const auto    MONITOR = monitor();
        const SCamera CAMERA  = live() ? m_camera : homeCamera();
        if (!MONITOR)
            return {};
        return {CAMERA.origin + (usableArea().middle() - MONITOR->m_position) / CAMERA.scale, Vector2D{}};
    }

    SCamera CCanvas::fitCamera() const {
        const auto WINDOWS = canvasWindows();
        if (WINDOWS.empty())
            return {homeCamera().origin, OVERVIEW_MAX_SCALE};

        CBox bounds = WINDOWS.front()->getFullWindowBoundingBox();
        for (const auto& window : WINDOWS) {
            const CBox   BOX  = window->getFullWindowBoundingBox();
            const double LEFT = std::min(bounds.x, BOX.x), TOP = std::min(bounds.y, BOX.y);
            const double RIGHT = std::max(bounds.x + bounds.w, BOX.x + BOX.w), BOTTOM = std::max(bounds.y + bounds.h, BOX.y + BOX.h);
            bounds = {LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
        }

        const CBox   AREA  = usableArea();
        const double SCALE = std::clamp(std::min({(AREA.w - 2.0 * OVERVIEW_MARGIN) / std::max(bounds.w, 1.0), (AREA.h - 2.0 * OVERVIEW_MARGIN) / std::max(bounds.h, 1.0),
                                                  OVERVIEW_MAX_SCALE}),
                                        OVERVIEW_MIN_SCALE, OVERVIEW_MAX_SCALE);

        return centredOn(bounds.middle(), SCALE);
    }

    SCamera CCanvas::cameraFor(PHLWINDOW window) const {
        const auto MONITOR = monitor();
        if (!MONITOR)
            return homeCamera();

        const CBox WINDOW = canvasBox(window);
        const CBox AREA   = usableArea();

        // Centred in the usable area; a window too big for it lines up with
        // the area's top-left corner instead.
        const double SCREENX = WINDOW.w <= AREA.w ? AREA.x + (AREA.w - WINDOW.w) / 2.0 : AREA.x;
        const double SCREENY = WINDOW.h <= AREA.h ? AREA.y + (AREA.h - WINDOW.h) / 2.0 : AREA.y;

        return {Vector2D{WINDOW.x - (SCREENX - MONITOR->m_position.x), WINDOW.y - (SCREENY - MONITOR->m_position.y)}, 1.0};
    }

    CBox CCanvas::screenBox(PHLWINDOW window) const {
        const auto MONITOR = monitor();
        if (!MONITOR || !window)
            return {};

        const CBox BOX = canvasBox(window);
        if (!live())
            return BOX;
        return {MONITOR->m_position + (BOX.pos() - m_camera.origin) * m_camera.scale, BOX.size() * m_camera.scale};
    }

    PHLWINDOW CCanvas::windowAt(const Vector2D& pos) const {
        const auto WINDOWS = canvasWindows();
        for (const auto& window : WINDOWS | std::views::reverse) {
            if (screenBox(window).containsPoint(pos))
                return window;
        }
        return nullptr;
    }

    bool CCanvas::pickAt(const Vector2D& pos) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return false;

        const auto WINDOW = windowAt(pos);
        if (WINDOW)
            glideTo(WINDOW);
        else
            close();
        return WINDOW != nullptr;
    }

    std::string CCanvas::stateJson() const {
        static constexpr const char* STATES[] = {"idle", "opening", "overview", "gliding"};

        std::string windows;
        for (const auto& window : canvasWindows()) {
            const CBox CANVAS = canvasBox(window);
            const CBox SCREEN = screenBox(window);
            if (!windows.empty())
                windows += ",";
            windows += std::format(R"({{"title":"{}","class":"{}","floating":{},"canvas":[{:.0f},{:.0f},{:.0f},{:.0f}],"screen":[{:.0f},{:.0f},{:.0f},{:.0f}]}})",
                                   jsonEscape(window->m_title), jsonEscape(window->m_class), window->m_isFloating, CANVAS.x + m_scrolled.x, CANVAS.y + m_scrolled.y, CANVAS.w,
                                   CANVAS.h, SCREEN.x, SCREEN.y, SCREEN.w, SCREEN.h);
        }

        const auto HOVER  = m_hover.lock();
        const auto TARGET = m_target.lock();
        return std::format(R"({{"state":"{}","scale":{:.3f},"origin":[{:.1f},{:.1f}],"scrolled":[{:.1f},{:.1f}],"hover":"{}","target":"{}","windows":[{}]}})",
                           STATES[m_state], m_camera.scale, m_camera.origin.x, m_camera.origin.y, m_scrolled.x, m_scrolled.y, HOVER ? jsonEscape(HOVER->m_title) : "",
                           TARGET ? jsonEscape(TARGET->m_title) : "", windows) +
            "\n";
    }

    void CCanvas::reset() {
        m_state     = STATE_IDLE;
        m_animating = false;
        m_target.reset();
        m_hover.reset();
        m_swallowedKeys.clear();
        m_camera = homeCamera();
    }
}
