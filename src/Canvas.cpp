#define WLR_USE_UNSTABLE

#include "Canvas.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
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
    constexpr double GLIDE_SECONDS      = 0.38;
    constexpr double CLOSE_SECONDS      = 0.28;
    constexpr double OVERVIEW_MARGIN    = 48.0;
    constexpr double OVERVIEW_MAX_SCALE = 0.9;
    constexpr double OVERVIEW_MIN_SCALE = 0.05;

    static double easeInOutCubic(double t) {
        return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
    }

    // Moves the canvas point at the centre of the view in a straight line while
    // the scale changes geometrically, so a zoom feels the same at every depth.
    static SCamera mix(const SCamera& a, const SCamera& b, double t, const Vector2D& view) {
        const double   scale = std::exp(std::lerp(std::log(a.scale), std::log(b.scale), t));
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

    void CCanvas::toggle() {
        switch (m_state) {
            case STATE_IDLE: open(); break;
            case STATE_OPENING:
            case STATE_OVERVIEW: close(); break;
            case STATE_GLIDING: break;
        }
    }

    void CCanvas::open() {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return;

        m_monitor   = MONITOR;
        m_workspace = MONITOR->m_activeWorkspace;
        m_hover     = Desktop::focusState()->window();
        m_target.reset();

        floatTiledWindows();

        m_camera = homeCamera();
        animateTo(fitCamera(), STATE_OPENING, OPEN_SECONDS);
    }

    void CCanvas::close() {
        if (m_state != STATE_OPENING && m_state != STATE_OVERVIEW)
            return;

        m_target.reset();
        animateTo(homeCamera(), STATE_GLIDING, CLOSE_SECONDS);
    }

    void CCanvas::glideTo(PHLWINDOW window) {
        if (!window || !live()) {
            close();
            return;
        }

        m_target = window;
        m_hover  = window;
        animateTo(cameraFor(window), STATE_GLIDING, GLIDE_SECONDS);
    }

    void CCanvas::animateTo(const SCamera& to, eState state, double seconds) {
        m_from        = m_camera;
        m_to          = to;
        m_animStart   = std::chrono::steady_clock::now();
        m_animSeconds = seconds;
        m_state       = state;

        if (const auto MONITOR = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(MONITOR);
    }

    void CCanvas::onPreRender(PHLMONITOR monitor) {
        if (!live() || monitor != m_monitor.lock())
            return;

        if (m_state == STATE_OPENING || m_state == STATE_GLIDING) {
            const double ELAPSED = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_animStart).count();
            const double T       = m_animSeconds > 0.0 ? std::clamp(ELAPSED / m_animSeconds, 0.0, 1.0) : 1.0;

            m_camera = mix(m_from, m_to, easeInOutCubic(T), monitor->m_size);

            if (T >= 1.0) {
                m_camera = m_to;
                if (m_state == STATE_OPENING)
                    m_state = STATE_OVERVIEW;
                else {
                    // Moving windows from inside a frame is unsafe; the camera
                    // holds the final view until the move lands.
                    m_animSeconds = 0.0;
                    g_pEventLoopManager->doLater([this] {
                        if (m_state == STATE_GLIDING)
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
        if (HOVER != m_hover.lock()) {
            m_hover = HOVER;
            if (const auto MONITOR = m_monitor.lock())
                g_pHyprRenderer->damageMonitor(MONITOR);
        }

        // Swallowed so apps don't see a pointer over their real, off-view
        // positions and focus doesn't follow the mouse.
        return true;
    }

    bool CCanvas::onMouseButton(uint32_t button, bool pressed) {
        if (!live())
            return false;

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
            return false; // chords stay keybinds, so the toggle key works

        m_swallowedKeys.push_back(keycode);
        return true;
    }

    bool CCanvas::live() const {
        return m_state != STATE_IDLE;
    }

    bool CCanvas::isCanvasWindow(PHLWINDOW window) const {
        if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned)
            return false;

        const auto WORKSPACE = m_workspace.lock();
        return WORKSPACE && window->m_workspace == WORKSPACE;
    }

    bool CCanvas::drawsOn(PHLMONITOR monitor) const {
        return live() && monitor && monitor == m_monitor.lock();
    }

    PHLWINDOW CCanvas::hovered() const {
        return m_hover.lock();
    }

    PHLWORKSPACE CCanvas::workspace() const {
        return m_workspace.lock();
    }

    double CCanvas::zoomedOut() const {
        return std::clamp((1.0 - m_camera.scale) / 0.25, 0.0, 1.0);
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
        const auto MONITOR = m_monitor.lock();
        return MONITOR ? MONITOR->logicalBoxMinusReserved() : CBox{};
    }

    SCamera CCanvas::homeCamera() const {
        const auto MONITOR = m_monitor.lock();
        return {MONITOR ? MONITOR->m_position : Vector2D{}, 1.0};
    }

    SCamera CCanvas::fitCamera() const {
        const auto MONITOR = m_monitor.lock();
        const auto WINDOWS = canvasWindows();
        if (!MONITOR || WINDOWS.empty())
            return {homeCamera().origin, OVERVIEW_MAX_SCALE};

        CBox bounds = WINDOWS.front()->getFullWindowBoundingBox();
        for (const auto& window : WINDOWS) {
            const CBox BOX    = window->getFullWindowBoundingBox();
            const double LEFT = std::min(bounds.x, BOX.x), TOP = std::min(bounds.y, BOX.y);
            const double RIGHT = std::max(bounds.x + bounds.w, BOX.x + BOX.w), BOTTOM = std::max(bounds.y + bounds.h, BOX.y + BOX.h);
            bounds = {LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
        }

        const CBox   AREA  = usableArea();
        const double SCALE = std::clamp(std::min({(AREA.w - 2.0 * OVERVIEW_MARGIN) / std::max(bounds.w, 1.0), (AREA.h - 2.0 * OVERVIEW_MARGIN) / std::max(bounds.h, 1.0),
                                                  OVERVIEW_MAX_SCALE}),
                                        OVERVIEW_MIN_SCALE, OVERVIEW_MAX_SCALE);

        return {bounds.middle() - (AREA.middle() - MONITOR->m_position) / SCALE, SCALE};
    }

    SCamera CCanvas::cameraFor(PHLWINDOW window) const {
        const auto MONITOR = m_monitor.lock();
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
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR || !window)
            return {};

        const CBox BOX = canvasBox(window);
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
            const CBox SCREEN = live() ? screenBox(window) : CANVAS;
            if (!windows.empty())
                windows += ",";
            windows += std::format(R"({{"title":"{}","class":"{}","floating":{},"canvas":[{:.0f},{:.0f},{:.0f},{:.0f}],"screen":[{:.0f},{:.0f},{:.0f},{:.0f}]}})",
                                   jsonEscape(window->m_title), jsonEscape(window->m_class), window->m_isFloating, CANVAS.x + m_scrolled.x, CANVAS.y + m_scrolled.y, CANVAS.w,
                                   CANVAS.h, SCREEN.x, SCREEN.y, SCREEN.w, SCREEN.h);
        }

        const auto HOVER = m_hover.lock();
        return std::format(R"({{"state":"{}","scale":{:.3f},"origin":[{:.1f},{:.1f}],"scrolled":[{:.1f},{:.1f}],"hover":"{}","windows":[{}]}})", STATES[m_state], m_camera.scale,
                           m_camera.origin.x, m_camera.origin.y, m_scrolled.x, m_scrolled.y, HOVER ? jsonEscape(HOVER->m_title) : "", windows) +
            "\n";
    }

    void CCanvas::reset() {
        m_state = STATE_IDLE;
        m_target.reset();
        m_hover.reset();
        m_swallowedKeys.clear();
        if (const auto MONITOR = m_monitor.lock())
            m_camera = homeCamera();
    }
}
