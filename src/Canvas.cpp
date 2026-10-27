#define WLR_USE_UNSTABLE

#include "Canvas.hpp"
#include "HotCorner.hpp"
#include "Later.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ranges>

namespace Expanse {

    using Desktop::View::IGeometric;

    // Timings and limits as on the Mac.
    constexpr double CAMERA_SECONDS     = 0.22; // zooming out, zoom keys, fit
    constexpr double DIVE_SECONDS       = 0.18; // from the canvas into a window, or back
    constexpr double HOP_SECONDS        = 0.30; // at 100%
    constexpr double PAN_SECONDS        = 0.25; // on the canvas, following the highlight
    constexpr int    BACK_FROM_FULLSCREEN_MS = 400; // for Hyprland to put a window back in its box
    constexpr double FIT_INSET          = 80.0; // canvas px around everything, when fitting
    constexpr double MIN_SCALE          = 0.03;
    constexpr double MAX_SCALE          = 1.5;
    constexpr double TIDY_MARGIN        = 16.0;
    constexpr double CLOSE_SIZE         = 26.0;
    constexpr double CLOSE_INSET        = 10.0;
    constexpr double CLOSE_MIN_WIDTH    = 220.0; // narrower on screen, a window shows no ×
    constexpr Vector2D MINIMAP_SIZE     = {208.0, 136.0};
    constexpr double   MINIMAP_MARGIN   = 16.0;
    constexpr auto     TEXT_SETTLE      = std::chrono::milliseconds(150); // still this long, labels are made at their size

    // Omarchy's theme says whether it's light (colors.toml's mode, or the
    // older light.mode marker beside it).
    static bool themeIsLight() {
        const char* STATE = std::getenv("XDG_STATE_HOME");
        const char* HOME  = std::getenv("HOME");
        const std::string THEME = STATE && *STATE ? std::string{STATE} + "/omarchy/current/theme" : HOME ? std::string{HOME} + "/.local/state/omarchy/current/theme" : "";
        if (THEME.empty())
            return false;
        if (std::ifstream{THEME + "/light.mode"})
            return true;
        std::ifstream colors{THEME + "/colors.toml"};
        for (std::string line; std::getline(colors, line);) {
            std::erase_if(line, [](char c) { return c == ' ' || c == '\t' || c == '\'' || c == '"'; });
            if (line == "mode=light")
                return true;
        }
        return false;
    }

    static double easeOutCubic(double t) {
        return 1.0 - std::pow(1.0 - t, 3.0);
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

    // Where the window is on the canvas: its goal, so a window still animating
    // in (or moving) is arranged by where it's going.
    static CBox canvasBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)};
    }

    // Where it's drawn this frame, mid-animation included.
    static CBox drawnBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_CURRENT), window->size(IGeometric::GEOMETRIC_CURRENT)};
    }

    static CBox unite(const CBox& a, const CBox& b) {
        const double X0 = std::min(a.x, b.x), Y0 = std::min(a.y, b.y);
        return {X0, Y0, std::max(a.x + a.w, b.x + b.w) - X0, std::max(a.y + a.h, b.y + b.h) - Y0};
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

    // Keyboard glides replace Hyprland's movefocus, which carries the pointer
    // onto the window it focuses (unless cursor:no_warps), so focus that
    // follows the mouse doesn't snap back to whatever slid under the pointer.
    // Mirrors its switchToWindow(), which plugins can't call.
    static void focusLikeMoveFocus(PHLWINDOW window) {
        static auto PNOWARPS     = CConfigValue<Config::INTEGER>("cursor:no_warps");
        static auto PFOLLOWMOUSE = CConfigValue<Config::INTEGER>("input:follow_mouse");

        if (const auto LAST = Desktop::focusState()->window(); LAST && !*PNOWARPS)
            LAST->m_relativeCursorCoordsOnLastWarp = g_pInputManager->getMouseCoordsInternal() - LAST->layoutBox().pos();

        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
        window->warpCursor();

        if (*PNOWARPS == 0 || *PFOLLOWMOUSE < 2) {
            g_pInputManager->m_forcedFocus = window;
            g_pInputManager->simulateMouseMovement();
            g_pInputManager->m_forcedFocus.reset();
        }
    }

    std::string appName(const PHLWINDOW& window) {
        std::string name = window->m_class;
        if (const auto DOT = name.rfind('.'); DOT != std::string::npos && DOT + 1 < name.size())
            name = name.substr(DOT + 1);
        if (!name.empty())
            name[0] = std::toupper(sc<unsigned char>(name[0]));
        return name.empty() ? window->m_title : name;
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

    // MARK: Modes

    void CCanvas::toggle() {
        if (m_paused) {
            resume();
            return;
        }
        switch (m_state) {
            case STATE_IDLE: open(); break;
            case STATE_OPENING:
            case STATE_OVERVIEW: close(); break;
            case STATE_GLIDING: break;
        }
    }

    bool CCanvas::begin(PHLMONITOR monitor) {
        if (m_paused)
            return false;
        const auto MONITOR = monitor ? monitor : Desktop::focusState()->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return false;

        m_monitor   = MONITOR;
        m_workspace = MONITOR->m_activeWorkspace;
        m_light     = themeIsLight();
        m_diving    = false;
        m_target.reset();
        m_hoverSpace.reset();
        m_selected.clear();
        m_drag = {};
        m_guides.clear();
        leaveFullscreen();
        floatTiledWindows();
        m_camera = homeCamera();
        return true;
    }

    void CCanvas::open(PHLMONITOR monitor) {
        if (m_state == STATE_OVERVIEW) {
            animateTo(fitCamera(OVERVIEW_MAX_SCALE), STATE_OVERVIEW, CAMERA_SECONDS);
            return;
        }
        if (live() || !begin(monitor))
            return;

        // As on the Mac, only the window the pointer moves onto is ringed.
        m_hover.reset();
        m_returnCamera = m_camera;
        animateTo(fitCamera(OVERVIEW_MAX_SCALE), STATE_OPENING, CAMERA_SECONDS);
    }

    void CCanvas::openAt100() {
        if (live() || !begin())
            return;

        m_hover.reset();
        m_returnCamera.reset();
        m_animating = false;
        m_state     = STATE_OVERVIEW;
        damage();
    }

    void CCanvas::close() {
        if (m_state != STATE_OPENING && m_state != STATE_OVERVIEW)
            return;

        m_target.reset();
        m_selected.clear();
        // Straight back where you were, unless you've moved around since zooming out.
        m_diving = true;
        animateTo(m_returnCamera.value_or(centredOn(viewCentre().pos(), 1.0)), STATE_GLIDING, DIVE_SECONDS);
    }

    void CCanvas::glideTo(PHLWINDOW window, bool fromKeyboard) {
        if (!window)
            return;

        const bool FROMIDLE = !live();
        // On the window's own monitor, which isn't always the focused one,
        // until the glide has started there.
        std::optional<SOnMonitor> here;
        if (FROMIDLE) {
            here.emplace(*this, window->m_monitor.lock());
            if (!begin(window->m_monitor.lock()))
                return;
        }

        if (!isCanvasWindow(window))
            return;

        const double SECONDS = FROMIDLE || m_state == STATE_GLIDING ? HOP_SECONDS : DIVE_SECONDS;

        // Too big for the screen, it shrinks to fit; it grows back on a bigger one.
        fitToScreen(window);

        m_target       = window;
        m_hover        = window;
        m_fromKeyboard = fromKeyboard;
        // From the canvas, it stays drawn as you dive in; a hop on from a
        // glide is a hop.
        m_diving = !FROMIDLE && m_state != STATE_GLIDING;
        m_selected.clear();
        // On top for the whole glide, not only once it lands.
        raise(window);

        animateTo(cameraFor(window), STATE_GLIDING, SECONDS);
    }

    void CCanvas::glideLater(PHLWINDOW window, bool fromKeyboard) {
        // Out of the event that asked for it: gliding floats and moves windows.
        later([this, ref = PHLWINDOWREF{window}, fromKeyboard] {
            if (const auto WINDOW = ref.lock(); WINDOW && !live() && m_autoCentre && offItsScreen(WINDOW))
                glideTo(WINDOW, fromKeyboard);
        });
    }

    void CCanvas::hop(eDirection direction) {
        if (m_paused)
            return;
        switch (m_state) {
            case STATE_GLIDING: {
                // Mid-glide: hop on from the window it's heading to, straight away.
                const auto CURRENT = m_target.lock();
                if (const auto NEXT = CURRENT ? nearest(direction, canvasBox(CURRENT), CURRENT) : nullptr)
                    glideTo(NEXT, true);
                return;
            }
            case STATE_OPENING:
            case STATE_OVERVIEW: {
                // On the canvas: the highlight moves and the view follows,
                // from the highlighted window, or the top one.
                auto FROM = m_hover.lock();
                if (!FROM || !isCanvasWindow(FROM)) {
                    const auto WINDOWS = canvasWindows();
                    FROM               = WINDOWS.empty() ? nullptr : WINDOWS.back();
                }
                const bool HAS  = FROM != nullptr;
                const auto NEXT = nearest(direction, HAS ? canvasBox(FROM) : viewCentre(), HAS ? FROM : nullptr);
                if (!NEXT)
                    return;
                m_hover = NEXT;
                cameraMovedByUser();
                animateTo(centredOn(canvasBox(NEXT).middle(), m_animating ? m_to.scale : m_camera.scale), STATE_OVERVIEW, PAN_SECONDS);
                return;
            }
            case STATE_IDLE: {
                // Every hop centres its window: one that only moved focus to a
                // window already on screen looked like nothing happened.
                const auto FOCUSED = Desktop::focusState()->window();
                const bool HAS     = isCanvasWindow(FOCUSED);
                // From a full-screen window, the way is measured from its spot
                // on the canvas, which it goes back to as the hop starts.
                const bool FULLSCREEN = HAS && Fullscreen::controller()->isFullscreen(FOCUSED);
                if (FULLSCREEN) {
                    // Nothing that way from where it was: it stays full screen, untouched.
                    if (const auto* ENTRY = tracked(FOCUSED);
                        ENTRY && !nearest(direction, CBox{ENTRY->home - scrolledFor(FOCUSED->m_workspace), ENTRY->preferred}, FOCUSED))
                        return;
                    if (!begin())
                        return;
                }
                const auto NEXT = nearest(direction, HAS ? canvasBox(FOCUSED) : viewCentre(), HAS ? FOCUSED : nullptr);
                if (NEXT && !m_autoCentre && !FULLSCREEN && fullyVisible(NEXT)) {
                    // Auto-centre off, as on the Mac: a window already on
                    // screen is focused where it is.
                    raise(NEXT);
                    focusLikeMoveFocus(NEXT);
                } else if (NEXT)
                    glideTo(NEXT, true);
                else if (FULLSCREEN)
                    restoreFullscreen();
                return;
            }
        }
    }

    bool CCanvas::pickAt(const Vector2D& pos) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return false;

        const auto WINDOW = windowAt(pos);
        if (WINDOW)
            glideTo(WINDOW);
        return WINDOW != nullptr;
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

    // MARK: Camera

    void CCanvas::animateTo(const SCamera& to, eState state, double seconds) {
        m_from        = m_camera;
        m_to          = to;
        m_animStart   = std::chrono::steady_clock::now();
        m_animSeconds = seconds;
        m_animating   = true;
        m_state       = state;
        m_animFrames  = {};
        damage();

        // Animations advance with the monitor's frames. One that stops
        // drawing (asleep, or headless) would leave the canvas up and holding
        // the pointer: once the time is up, it ends anyway.
        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!m_animationTimer)
            m_animationTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void* data) {
                    sc<CCanvas*>(data)->advance(true);
                    return 0;
                },
                this);
        wl_event_source_timer_update(m_animationTimer, sc<int>(seconds * 1000) + 250);
    }

    void CCanvas::advance(bool late) {
        if (!m_animating)
            return;
        const auto   MONITOR = m_monitor.lock();
        const auto   NOW     = std::chrono::steady_clock::now();
        const double ELAPSED = std::chrono::duration<double>(NOW - m_animStart).count();
        const double T       = late || m_animSeconds <= 0.0 ? 1.0 : std::clamp(ELAPSED / m_animSeconds, 0.0, 1.0);
        if (!late) {
            m_cameraChangedAt = NOW;
            if (m_animFrames.frames++ == 0)
                m_animFrames.first = NOW;
            else
                m_animFrames.maxGap = std::max(m_animFrames.maxGap, std::chrono::duration<double>(NOW - m_animFrames.last).count());
            m_animFrames.last = NOW;
        }

        m_camera = mix(m_from, m_to, easeOutCubic(T), MONITOR ? MONITOR->m_size : Vector2D{1, 1});
        if (T < 1.0)
            return;

        m_camera    = m_to;
        m_animating = false;
        const double FIRST = m_animFrames.frames ? std::chrono::duration<double>(m_animFrames.first - m_animStart).count() : ELAPSED;
        m_lastAnimation    = std::format("first frame after {}ms, {} frames, max gap {}ms{}", sc<int>(FIRST * 1000), m_animFrames.frames,
                                         sc<int>(m_animFrames.maxGap * 1000), late ? ", ended by the timer" : "");
        if (late || m_animFrames.maxGap > 0.034 || FIRST > 0.034)
            Log::logger->log(Log::INFO, "[expanse] slow: animation {}", m_lastAnimation);
        if (m_state == STATE_OPENING)
            m_state = STATE_OVERVIEW;
        else if (m_state == STATE_GLIDING) {
            // Moving windows from inside a frame is unsafe; the camera holds
            // the final view until the move lands.
            later([this] {
                if (m_state == STATE_GLIDING && !m_animating)
                    commit();
            });
        }
        damage();
    }

    void CCanvas::setCamera(const SCamera& camera) {
        m_camera          = camera;
        m_camera.scale    = std::clamp(camera.scale, MIN_SCALE, MAX_SCALE);
        m_animating       = false;
        m_diving          = false;
        m_cameraChangedAt = std::chrono::steady_clock::now();
        if (m_state == STATE_OPENING)
            m_state = STATE_OVERVIEW;
        damage();
    }

    void CCanvas::cameraMovedByUser() {
        m_returnCamera.reset();
    }

    void CCanvas::damage() const {
        if (const auto MONITOR = m_monitor.lock())
            g_pHyprRenderer->damageMonitor(MONITOR);
    }

    bool CCanvas::cameraMoving() const {
        return m_animating || m_coast.active || std::chrono::steady_clock::now() - m_cameraChangedAt < TEXT_SETTLE;
    }

    void CCanvas::wantFrame() {
        m_frameWanted = true;
    }

    bool CCanvas::takeFrameRequest(const PHLMONITOR& monitor) {
        if (!live() || monitor != m_monitor.lock()) {
            m_frameWanted = m_frameWanted && live();
            return false;
        }
        // Moving, one more once it's still (labels at their size), and any asked for.
        const bool WANT = m_animating || m_coast.active || m_frameWanted || cameraMoving();
        m_frameWanted   = false;
        return WANT;
    }

    bool CCanvas::interruptDive() {
        // As on the Mac: a click, scroll or pinch while diving in stops the
        // camera where it is, still on the canvas.
        if (m_state != STATE_GLIDING || !m_diving || !m_animating)
            return false;
        m_target.reset();
        m_state = STATE_OVERVIEW;
        setCamera(m_camera);
        return true;
    }

    void CCanvas::pan(const Vector2D& screenDelta) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        cameraMovedByUser();
        setCamera({m_camera.origin - screenDelta / m_camera.scale, m_camera.scale});
    }

    void CCanvas::zoomBy(double factor, const Vector2D& anchor) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        cameraMovedByUser();
        setCamera(zoomedAround(m_camera, m_camera.scale * factor, anchor));
    }

    void CCanvas::zoomTo(double scale) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        const auto MONITOR = monitor();
        if (!MONITOR)
            return;
        cameraMovedByUser();
        const SCamera FROM = m_animating ? m_to : m_camera;
        animateTo(zoomedAround(FROM, scale, MONITOR->m_position + MONITOR->m_size / 2.0), STATE_OVERVIEW, CAMERA_SECONDS);
    }

    void CCanvas::fit() {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        cameraMovedByUser();
        animateTo(fitCamera(1.0), STATE_OVERVIEW, CAMERA_SECONDS);
    }

    void CCanvas::centreOn(const Vector2D& canvasPoint) {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        cameraMovedByUser();
        setCamera(centredOn(canvasPoint, m_camera.scale));
    }

    void CCanvas::onPreRender(PHLMONITOR monitor) {
        if (!m_paused)
            followWorkspaces();
        if (!live() || monitor != m_monitor.lock())
            return;

        advance(false);
        coast();
        updateCursor();

        // Hyprland reports damage in untransformed coordinates, so while the
        // camera is in use the whole monitor is redrawn every frame.
        g_pHyprRenderer->damageMonitor(monitor);
    }

    void CCanvas::settle(const SCamera& camera) {
        // The real windows move so the camera's view (at 100%) is the screen.
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR)
            return;

        const Vector2D DELTA = camera.origin - MONITOR->m_position;
        if (DELTA.size() <= 0.5)
            return;

        for (const auto& window : canvasWindows()) {
            if (Fullscreen::controller()->isFullscreen(window))
                continue;

            const auto TARGET = window->layoutTarget();
            if (!TARGET || !TARGET->floating())
                continue;

            noteOriginal(window);
            CBox box = TARGET->position();
            box.translate(-DELTA);
            g_layoutManager->setTargetGeom(box, TARGET);
            TARGET->warpPositionSize();
        }
        if (const auto WORKSPACE = m_workspace.lock())
            m_scrolled[WORKSPACE->m_name] = scrolledFor(WORKSPACE) + DELTA;
    }

    void CCanvas::commit() {
        const auto MONITOR = m_monitor.lock();
        if (!MONITOR) {
            reset();
            return;
        }

        settle(m_camera);

        // You went to another workspace as the glide ended (before the canvas
        // could follow): focusing the window would take you back.
        if (MONITOR->m_activeWorkspace != m_workspace.lock()) {
            reset();
            scheduleSave();
            return;
        }

        m_camera = homeCamera();
        m_state  = STATE_IDLE;
        m_diving = false;
        m_drag   = {};
        m_guides.clear();
        m_selected.clear();

        const auto TARGET     = m_target.lock();
        const auto FULLSCREEN = m_fullscreen.window.lock();
        if ((!TARGET || !TARGET->m_isMapped || TARGET == FULLSCREEN) && FULLSCREEN && FULLSCREEN->m_isMapped) {
            // Back to the full-screen window you zoomed out from.
            Desktop::focusState()->fullWindowFocus(FULLSCREEN, Desktop::FOCUS_REASON_CLICK);
            restoreFullscreen();
        } else if (!TARGET || !TARGET->m_isMapped) {
            // Back without picking one: the focused window if it's in view,
            // else the top one that is.
            const auto FOCUSED = Desktop::focusState()->window();
            const CBox AREA    = usableArea();
            const auto SHOWN   = [&](const PHLWINDOW& w) { return !canvasBox(w).intersection(AREA).empty(); };
            if (!(FOCUSED && isCanvasWindow(FOCUSED) && SHOWN(FOCUSED))) {
                for (const auto& window : canvasWindows() | std::views::reverse) {
                    if (SHOWN(window)) {
                        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
                        break;
                    }
                }
            }
        } else {
            if (m_fromKeyboard)
                focusLikeMoveFocus(TARGET);
            else
                Desktop::focusState()->fullWindowFocus(TARGET, Desktop::FOCUS_REASON_CLICK);
            raise(TARGET);
        }

        m_fullscreen = {};
        m_target.reset();
        m_hover.reset();
        g_pHyprRenderer->damageMonitor(MONITOR);
        updateCursor();
        scheduleSave();
        placeUnplaced();
    }

    void CCanvas::placeUnplaced() {
        // Windows that opened while the canvas couldn't place them.
        const auto WAITING = std::move(m_unplaced);
        m_unplaced.clear();
        for (const auto& ref : WAITING)
            later([this, ref] {
                if (const auto WINDOW = ref.lock(); WINDOW && !m_paused)
                    placeNewWindow(WINDOW, nullptr);
            });
    }

    // MARK: Windows

    void CCanvas::floatTiledWindows() {
        // Record every tiled box first: floating one window makes the layout
        // re-tile the rest.
        std::vector<std::pair<SP<Layout::ITarget>, CBox>> tiled;
        for (const auto& window : canvasWindows()) {
            const auto TARGET = window->layoutTarget();
            if (TARGET && !TARGET->floating() && !Fullscreen::controller()->isFullscreen(window)) {
                noteOriginal(window);
                // Tiled since the canvas first saw it (you tiled it): that's where it goes back to.
                if (auto* entry = tracked(window)) {
                    entry->wasTiled = true;
                    entry->box      = TARGET->position();
                }
                tiled.emplace_back(TARGET, TARGET->position());
            }
        }

        for (const auto& [target, box] : tiled)
            g_layoutManager->changeFloatingMode(target);

        for (const auto& [target, box] : tiled) {
            g_layoutManager->setTargetGeom(box, target);
            target->warpPositionSize();
        }
    }

    void CCanvas::leaveFullscreen() {
        const auto WORKSPACE = m_workspace.lock();
        const auto WINDOW    = WORKSPACE ? Fullscreen::controller()->getFullscreenWindow(WORKSPACE) : nullptr;
        if (!WINDOW)
            return;
        // Through the handler that has it, which put it there: that one knows the size and spot to go back to.
        m_fullscreen          = {WINDOW, Fullscreen::controller()->getFullscreenModes(WINDOW), Fullscreen::controller()->layoutManagedFS(WINDOW)};
        m_ownFullscreenChange = true;
        Fullscreen::controller()->setFullscreenMode(WINDOW, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE, m_fullscreen.layoutAware);
        m_ownFullscreenChange = false;
    }

    void CCanvas::restoreFullscreen() {
        const auto WINDOW = m_fullscreen.window.lock();
        const auto SAVED  = m_fullscreen;
        m_fullscreen      = {};
        if (WINDOW && WINDOW->m_isMapped && WINDOW->m_workspace == m_workspace.lock()) {
            m_ownFullscreenChange = true;
            Fullscreen::controller()->setFullscreenMode(WINDOW, SAVED.mode.internal, SAVED.mode.client, SAVED.layoutAware);
            m_ownFullscreenChange = false;
        }
    }

    void CCanvas::onFullscreen(PHLWINDOW window) {
        // A window back from full screen (or maximised) is what you're
        // looking at, with the keyboard, wherever the view has got to
        // meanwhile: the view goes to it, as on the Mac, once it's back in its
        // box. Not when the canvas takes it out of full screen or back, nor
        // for a request Hyprland didn't carry out.
        if (!window)
            return;
        std::erase_if(m_wentFullscreen, [](const PHLWINDOWREF& ref) { return !ref.lock(); });
        const bool WAS = std::erase_if(m_wentFullscreen, [&](const PHLWINDOWREF& ref) { return ref.lock() == window; }) > 0;
        if (Fullscreen::controller()->isFullscreen(window)) {
            m_wentFullscreen.emplace_back(window);
            return;
        }
        if (!WAS || m_paused || m_ownFullscreenChange || live() || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        m_backFromFullscreen = window;
        if (!m_fullscreenTimer)
            m_fullscreenTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void* data) {
                    auto*      canvas = sc<CCanvas*>(data);
                    const auto WINDOW = canvas->m_backFromFullscreen.lock();
                    canvas->m_backFromFullscreen.reset();
                    if (WINDOW && !canvas->m_paused && !canvas->live() && WINDOW->m_isMapped && !Fullscreen::controller()->isFullscreen(WINDOW) && WINDOW->m_workspace &&
                        WINDOW->m_workspace->isVisible())
                        canvas->glideTo(WINDOW);
                    return 0;
                },
                this);
        wl_event_source_timer_update(m_fullscreenTimer, BACK_FROM_FULLSCREEN_MS);
    }

    PHLWINDOW CCanvas::topWindowInView(PHLWINDOW excluding) const {
        const CBox VIEW = live() ? viewInCanvas() : usableArea();
        for (const auto& window : canvasWindows() | std::views::reverse)
            if (window != excluding && !canvasBox(window).intersection(VIEW).empty())
                return window;
        return nullptr;
    }

    void CCanvas::onWindowOpen(PHLWINDOW window) {
        // A new standard window opens in free space beside what you were
        // working on (the top window in view), instead of on top of it.
        // Dialogs keep the spot their app gives them.
        if (m_paused || !window || window->parent() || window->isModal() || window->isX11OverrideRedirect() || !window->m_workspace)
            return;

        // On its own workspace, showing or not (a rule or an app can open it
        // on another). With the canvas up on another workspace there's no
        // view there to open it beside; it waits (placeNewWindow).
        PHLWINDOW ANCHOR;
        if (!live() || window->m_workspace == m_workspace.lock()) {
            SLookAt here{*this, window->m_workspace};
            ANCHOR = topWindowInView(window);
        }
        // Out of the event: Hyprland is still mapping the window.
        later([this, ref = PHLWINDOWREF{window}, anchor = PHLWINDOWREF{ANCHOR}] {
            if (const auto WINDOW = ref.lock())
                placeNewWindow(WINDOW, anchor.lock());
        });
    }

    void CCanvas::placeNewWindow(PHLWINDOW window, PHLWINDOW anchor) {
        if (!window->m_isMapped || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
            return;
        // Mid-glide, or with the canvas up on another workspace: once the
        // canvas is back at 100% (or showing its workspace).
        if (m_state == STATE_GLIDING || (live() && window->m_workspace != m_workspace.lock())) {
            m_unplaced.emplace_back(window);
            return;
        }

        const auto TARGET = window->layoutTarget();
        if (!TARGET)
            return;

        SLookAt    here{*this, window->m_workspace};
        const bool SHOWING = window->m_workspace->isVisible();

        // A window that had a spot last time (an app opened again, or after a
        // restart, on any workspace) goes back to it.
        if (placeFromSaved(window, false)) {
            scheduleSave();
            if (SHOWING && isCanvasWindow(window) && !live() && !fullyVisible(window))
                glideTo(window);
            return;
        }

        if (!isCanvasWindow(window))
            return;

        noteOriginal(window);
        // examples/expanse.lua floats windows from their first frame, at their
        // own size; without it, a new window floats here.
        if (!TARGET->floating())
            g_layoutManager->changeFloatingMode(TARGET);

        std::vector<CBox> others;
        for (const auto& other : canvasWindows())
            if (other != window)
                others.push_back(canvasBox(other));

        const CBox     BOX     = canvasBox(window);
        const Vector2D NEAR    = anchor && isCanvasWindow(anchor) ? canvasBox(anchor).middle() : viewCentre().pos();
        const CBox     VISIBLE = live() ? viewInCanvas() : usableArea();
        const auto     SPOT    = Arrange::freeSpot(BOX.size(), others, NEAR, VISIBLE, !m_autoCentre);
        if (!SPOT)
            return; // the first window, or nowhere free: where Hyprland put it

        moveWindow(window, *SPOT);
        scheduleSave();

        // Off screen: the canvas glides to it, as to any window you go to.
        if (SHOWING && !live() && !fullyVisible(window))
            glideTo(window);
    }

    void CCanvas::raise(PHLWINDOW window) {
        Desktop::windowState()->raise(window);
        if (const auto TARGET = window->layoutTarget())
            g_layoutManager->bringTargetToTop(TARGET); // the current tab, for a group
    }

    void CCanvas::moveWindow(PHLWINDOW window, const Vector2D& canvasPos) {
        const auto TARGET = window ? window->layoutTarget() : nullptr;
        if (!TARGET || !TARGET->floating())
            return;

        noteOriginal(window);
        // The layout box includes the border; keep its offset from the window.
        CBox       box    = TARGET->position();
        const auto OFFSET = box.pos() - canvasBox(window).pos();
        box.x             = std::round(canvasPos.x + OFFSET.x);
        box.y             = std::round(canvasPos.y + OFFSET.y);
        g_layoutManager->setTargetGeom(box, TARGET);
        TARGET->warpPositionSize();
        damage();
    }

    void CCanvas::tidy() {
        if (m_state == STATE_GLIDING)
            return;
        if (!live()) {
            openAt100();
            if (!live())
                return;
        }

        const auto WINDOWS = canvasWindows();
        if (WINDOWS.empty())
            return;

        // As on the Mac: sizes back to what you chose (capped at the screen),
        // overlaps pushed apart, groups pulled back to the standard gap.
        for (const auto& window : WINDOWS)
            noteOriginal(window);
        noteSizes();
        updateHome();
        fitLayoutToScreen(TIDY_MARGIN, true);

        cameraMovedByUser();
        animateTo(fitCamera(1.0), STATE_OVERVIEW, CAMERA_SECONDS);
        scheduleSave();
    }

    void CCanvas::selectAll() {
        if (m_state != STATE_OVERVIEW && m_state != STATE_OPENING)
            return;
        m_selected.clear();
        for (const auto& window : canvasWindows())
            m_selected.emplace_back(window);
        damage();
    }

    void CCanvas::closeWindows(const std::vector<PHLWINDOW>& windows) {
        // Each window's own close request, so apps ask about unsaved changes as usual.
        for (const auto& window : windows)
            if (window)
                window->sendClose();
        m_selected.clear();
        damage();
    }

    void CCanvas::setAutoCentre(bool on) {
        m_autoCentre = on;
        scheduleSave();
    }

    bool CCanvas::autoCentre() const {
        return m_autoCentre;
    }

    void CCanvas::setWallpaper(std::optional<bool> on) {
        m_wallpaper = on;
        damage();
        scheduleSave();
    }

    bool CCanvas::wallpaper() const {
        static auto PWALLPAPER = CConfigValue<Config::BOOL>("plugin:expanse:wallpaper");
        return m_wallpaper.value_or(*PWALLPAPER);
    }

    void CCanvas::setHotCorner(std::optional<std::string> corner) {
        g_hotCorner.choice = std::move(corner);
        scheduleSave();
    }

    std::vector<PHLWINDOW> CCanvas::selection() const {
        std::vector<PHLWINDOW> out;
        for (const auto& ref : m_selected)
            if (const auto WINDOW = ref.lock(); WINDOW && isCanvasWindow(WINDOW))
                out.push_back(WINDOW);
        return out;
    }

    void CCanvas::clearSelection() {
        m_selected.clear();
        damage();
    }

    void CCanvas::toggleSelected(PHLWINDOW window) {
        const auto IT = std::ranges::find_if(m_selected, [&](const PHLWINDOWREF& ref) { return ref.lock() == window; });
        if (IT != m_selected.end())
            m_selected.erase(IT);
        else
            m_selected.emplace_back(window);
        damage();
    }

    // MARK: Queries

    bool CCanvas::live() const {
        return m_state != STATE_IDLE;
    }

    bool CCanvas::showsCanvas() const {
        return m_state == STATE_OPENING || m_state == STATE_OVERVIEW;
    }

    bool CCanvas::decorated() const {
        return showsCanvas() || (m_state == STATE_GLIDING && m_diving);
    }

    bool CCanvas::lightTheme() const {
        return m_light;
    }

    PHLMONITOR CCanvas::monitor() const {
        if (live())
            return m_monitor.lock();
        if (const auto LAYOUT = m_layoutMonitor.lock())
            return LAYOUT;
        if (const auto LOOKING = m_layoutWorkspace.lock())
            return LOOKING->m_monitor.lock();
        return Desktop::focusState()->monitor();
    }

    PHLWORKSPACE CCanvas::workspace() const {
        if (live())
            return m_workspace.lock();
        if (const auto LOOKING = m_layoutWorkspace.lock())
            return LOOKING;
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

    double CCanvas::scale() const {
        return live() ? m_camera.scale : 1.0;
    }

    bool CCanvas::isSelected(PHLWINDOW window) const {
        return std::ranges::any_of(m_selected, [&](const PHLWINDOWREF& ref) { return ref.lock() == window; });
    }

    bool CCanvas::showsClose(PHLWINDOW window) const {
        // Hovering a window in a selection shows × on every selected window: it closes them all.
        if ((m_state != STATE_OVERVIEW && !(m_state == STATE_GLIDING && m_diving)) || m_drag.moved || screenBox(window).w < CLOSE_MIN_WIDTH)
            return false;
        const auto HOVER = m_hover.lock();
        if (!HOVER)
            return false;
        if (window == HOVER)
            return true;
        return selection().size() > 1 && isSelected(HOVER) && isSelected(window);
    }

    std::optional<CBox> CCanvas::selectionBounds() const {
        const auto SELECTED = selection();
        if (SELECTED.size() <= 1)
            return std::nullopt;
        CBox out = canvasBox(SELECTED.front());
        for (const auto& window : SELECTED)
            out = unite(out, canvasBox(window));
        return out;
    }

    std::optional<CBox> CCanvas::marquee() const {
        if (m_drag.kind != SDrag::DRAG_MARQUEE || !m_drag.moved)
            return std::nullopt;
        const Vector2D A = m_drag.start, B = m_drag.last;
        return CBox{std::min(A.x, B.x), std::min(A.y, B.y), std::abs(B.x - A.x), std::abs(B.y - A.y)};
    }

    const std::vector<Arrange::SGuide>& CCanvas::guides() const {
        return m_guides;
    }

    CBox SMinimap::toScreen(const CBox& canvas) const {
        return {offset.x + (canvas.x - bounds.x) * scale, offset.y + (canvas.y - bounds.y) * scale, std::max(canvas.w * scale, 1.0), std::max(canvas.h * scale, 1.0)};
    }

    Vector2D SMinimap::toCanvas(const Vector2D& screen) const {
        return {(screen.x - offset.x) / scale + bounds.x, (screen.y - offset.y) / scale + bounds.y};
    }

    static SMinimap makeMinimap(const PHLMONITOR& monitor, const CBox& viewport, std::vector<CBox> windows) {
        SMinimap map;
        map.box      = {monitor->m_position.x + monitor->m_size.x - MINIMAP_MARGIN - MINIMAP_SIZE.x, monitor->m_position.y + monitor->m_size.y - MINIMAP_MARGIN - MINIMAP_SIZE.y,
                        MINIMAP_SIZE.x, MINIMAP_SIZE.y};
        map.area     = {map.box.x + 10, map.box.y + 10, map.box.w - 20, map.box.h - 36};
        map.viewport = viewport;
        map.windows  = std::move(windows);
        CBox bounds  = map.viewport;
        for (const auto& window : map.windows)
            bounds = unite(bounds, window);
        map.bounds = {bounds.x - bounds.w * 0.04, bounds.y - bounds.h * 0.04, bounds.w * 1.08, bounds.h * 1.08};
        map.scale  = std::min(map.area.w / std::max(map.bounds.w, 1.0), map.area.h / std::max(map.bounds.h, 1.0));
        map.offset = {map.area.x + (map.area.w - map.bounds.w * map.scale) / 2, map.area.y + (map.area.h - map.bounds.h * map.scale) / 2};
        return map;
    }

    std::optional<SMinimap> CCanvas::minimap() const {
        const auto MONITOR = monitor();
        if (!MONITOR || !decorated())
            return std::nullopt;
        std::vector<CBox> windows;
        for (const auto& window : canvasWindows())
            windows.push_back(canvasBox(window));
        return makeMinimap(MONITOR, viewInCanvas(), std::move(windows));
    }

    std::optional<SMinimap> CCanvas::desktopMinimap(const PHLMONITOR& monitor) const {
        const auto WORKSPACE = monitor ? monitor->m_activeWorkspace : nullptr;
        if (m_paused || !WORKSPACE || WORKSPACE->m_isSpecialWorkspace || (live() && monitor == m_monitor.lock() && decorated()))
            return std::nullopt;
        std::vector<CBox> windows;
        for (const auto& window : Desktop::windowState()->windows())
            if (window->m_isMapped && !window->m_pinned && window->m_workspace == WORKSPACE && !Fullscreen::controller()->isFullscreen(window))
                windows.push_back(canvasBox(window));
        // Mid-glide, the view moves over it.
        const bool GLIDING = live() && monitor == m_monitor.lock();
        return makeMinimap(monitor, GLIDING ? viewInCanvas() : CBox{monitor->m_position, monitor->m_size}, std::move(windows));
    }

    std::optional<SSpacesBar> CCanvas::desktopSpacesBar(const PHLMONITOR& monitor) const {
        const auto MAP = desktopMinimap(monitor);
        return MAP ? spacesBarBeside(monitor, monitor->m_activeWorkspace, *MAP) : std::nullopt;
    }

    void CCanvas::damageDesktopMaps() const {
        for (const auto& monitor : State::monitorState()->monitors()) {
            const auto MAP = monitor && monitor->m_enabled ? desktopMinimap(monitor) : std::nullopt;
            if (!MAP)
                continue;
            CBox area = MAP->box;
            if (const auto BAR = spacesBarBeside(monitor, monitor->m_activeWorkspace, *MAP))
                area = unite(area, BAR->box.copy().expand(20));
            g_pHyprRenderer->damageBox(area);
        }
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
        return usableAreaOf(monitor());
    }

    CBox CCanvas::usableAreaOf(const PHLMONITOR& monitor) const {
        if (!monitor)
            return {};
        CBox area = monitor->logicalBoxMinusReserved();
        if (m_screenOverride) {
            area.w = m_screenOverride->x;
            area.h = m_screenOverride->y;
        }
        return area;
    }

    bool CCanvas::onCanvasMonitor(const Vector2D& pos) const {
        const auto MONITOR = m_monitor.lock();
        return live() && MONITOR && CBox{MONITOR->m_position, MONITOR->m_size}.containsPoint(pos);
    }

    bool CCanvas::offItsScreen(const PHLWINDOW& window) {
        // Judged on its own monitor: a window parked over another monitor
        // takes Hyprland's monitor focus there with it.
        if (!window)
            return false;
        SOnMonitor here{*this, window->m_monitor.lock()};
        return isCanvasWindow(window) && !fullyVisible(window);
    }

    bool CCanvas::fullyVisible(PHLWINDOW window) const {
        // Full screen covers the bar's strip too, and is on screen by definition.
        if (Fullscreen::controller()->isFullscreen(window))
            return true;
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

    SCamera CCanvas::zoomedAround(const SCamera& camera, double scale, const Vector2D& anchor) const {
        const auto MONITOR = monitor();
        if (!MONITOR)
            return camera;
        // The canvas point under the anchor stays put.
        const double   S     = std::clamp(scale, MIN_SCALE, MAX_SCALE);
        const Vector2D LOCAL = anchor - MONITOR->m_position;
        const Vector2D POINT = camera.origin + LOCAL / camera.scale;
        return {POINT - LOCAL / S, S};
    }

    CBox CCanvas::viewCentre() const {
        const auto    MONITOR = monitor();
        const SCamera CAMERA  = live() ? m_camera : homeCamera();
        if (!MONITOR)
            return {};
        return {CAMERA.origin + (usableArea().middle() - MONITOR->m_position) / CAMERA.scale, Vector2D{}};
    }

    CBox CCanvas::viewInCanvas() const {
        const auto MONITOR = monitor();
        if (!MONITOR)
            return {};
        const Vector2D A = toCanvas(MONITOR->m_position), B = toCanvas(MONITOR->m_position + MONITOR->m_size);
        return {A, B - A};
    }

    SCamera CCanvas::fitCamera(double maxScale) const {
        const auto WINDOWS = canvasWindows();
        if (WINDOWS.empty())
            return centredOn(viewCentre().pos(), 0.5);

        CBox bounds = canvasBox(WINDOWS.front());
        for (const auto& window : WINDOWS)
            bounds = unite(bounds, canvasBox(window));
        bounds.expand(FIT_INSET);

        const CBox   AREA  = usableArea();
        const double SCALE = std::max(std::min({AREA.w / std::max(bounds.w, 1.0), AREA.h / std::max(bounds.h, 1.0), maxScale}), MIN_SCALE);
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

        return {Vector2D{std::round(WINDOW.x - (SCREENX - MONITOR->m_position.x)), std::round(WINDOW.y - (SCREENY - MONITOR->m_position.y))}, 1.0};
    }

    Vector2D CCanvas::toScreen(const Vector2D& canvasPoint) const {
        const auto MONITOR = monitor();
        if (!MONITOR || !live())
            return canvasPoint;
        return MONITOR->m_position + (canvasPoint - m_camera.origin) * m_camera.scale;
    }

    Vector2D CCanvas::toCanvas(const Vector2D& screenPoint) const {
        const auto MONITOR = monitor();
        if (!MONITOR || !live())
            return screenPoint;
        return m_camera.origin + (screenPoint - MONITOR->m_position) / m_camera.scale;
    }

    CBox CCanvas::screenBox(PHLWINDOW window) const {
        if (!window)
            return {};
        const CBox BOX = drawnBox(window);
        return {toScreen(BOX.pos()), BOX.size() * scale()};
    }

    CBox CCanvas::closeButton(const CBox& screen) {
        return {screen.x + screen.w - CLOSE_INSET - CLOSE_SIZE, screen.y + CLOSE_INSET, CLOSE_SIZE, CLOSE_SIZE};
    }

    PHLWINDOW CCanvas::windowAt(const Vector2D& pos) const {
        if (const auto MAP = minimap(); MAP && MAP->box.containsPoint(pos))
            return nullptr;
        if (const auto BAR = spacesBar(); BAR && BAR->box.containsPoint(pos))
            return nullptr;
        const auto WINDOWS = canvasWindows();
        for (const auto& window : WINDOWS | std::views::reverse) {
            if (screenBox(window).containsPoint(pos))
                return window;
        }
        return nullptr;
    }

    std::string CCanvas::stateJson() const {
        static constexpr const char* STATES[] = {"idle", "opening", "overview", "gliding"};

        std::string windows;
        for (const auto& window : canvasWindows()) {
            const CBox CANVAS = canvasBox(window);
            const CBox SCREEN = screenBox(window);
            if (!windows.empty())
                windows += ",";
            windows += std::format(R"({{"title":"{}","class":"{}","floating":{},"selected":{},"canvas":[{:.0f},{:.0f},{:.0f},{:.0f}],"screen":[{:.0f},{:.0f},{:.0f},{:.0f}]}})",
                                   jsonEscape(window->m_title), jsonEscape(window->m_class), window->m_isFloating, isSelected(window), CANVAS.x + scrolledFor(window->m_workspace).x,
                                   CANVAS.y + scrolledFor(window->m_workspace).y, CANVAS.w, CANVAS.h, SCREEN.x, SCREEN.y, SCREEN.w, SCREEN.h);
        }

        std::string spaces;
        if (const auto BAR = spacesBar())
            for (const auto& tile : BAR->tiles)
                spaces += std::format(R"({}{{"name":"{}","windows":{},"current":{},"hovered":{},"tile":[{:.0f},{:.0f},{:.0f},{:.0f}]}})", spaces.empty() ? "" : ",",
                                      jsonEscape(tile.name), tile.windows.size(), tile.current, tile.hovered, tile.box.x, tile.box.y, tile.box.w, tile.box.h);

        const auto HOVER     = m_hover.lock();
        const auto TARGET    = m_target.lock();
        const auto WORKSPACE = workspace();
        const auto MARQUEE   = marquee();
        const auto MAP       = minimap();
        const auto HINT      = m_offscreenHint.lock();
        const auto SCROLLED  = scrolledFor(WORKSPACE);
        return std::format(R"({{"state":"{}","paused":{},"workspace":"{}","scale":{:.3f},"origin":[{:.1f},{:.1f}],"scrolled":[{:.1f},{:.1f}],"autoCentre":{},"wallpaper":{},"hotCorner":"{}","hover":"{}","target":"{}","marquee":{},"minimap":{},"offscreenNotice":"{}","lastAnimation":"{}","windows":[{}],"spaces":[{}]}})",
                           STATES[m_state], m_paused, WORKSPACE ? jsonEscape(WORKSPACE->m_name) : "", m_camera.scale, m_camera.origin.x, m_camera.origin.y, SCROLLED.x,
                           SCROLLED.y, m_autoCentre, wallpaper(), jsonEscape(g_hotCorner.corner()), HOVER ? jsonEscape(HOVER->m_title) : "", TARGET ? jsonEscape(TARGET->m_title) : "",
                           MARQUEE ? std::format("[{:.1f},{:.1f},{:.1f},{:.1f}]", MARQUEE->x, MARQUEE->y, MARQUEE->w, MARQUEE->h) : "null",
                           MAP ? std::format("[{:.0f},{:.0f},{:.0f},{:.0f}]", MAP->box.x, MAP->box.y, MAP->box.w, MAP->box.h) : "null", HINT ? jsonEscape(HINT->m_class) : "", m_lastAnimation,
                           windows, spaces) +
            "\n";
    }

    void CCanvas::reset() {
        // Leaving any other way than picking a window: a full-screen window
        // gets full screen back.
        if (live())
            restoreFullscreen();
        m_state     = STATE_IDLE;
        m_animating = false;
        m_target.reset();
        m_hover.reset();
        m_hoverSpace.reset();
        m_selected.clear();
        m_drag = {};
        m_guides.clear();
        m_swallowedKeys.clear();
        m_spaceHeld = false;
        m_pinching  = false;
        m_diving    = false;
        m_camera    = homeCamera();
        stopCoasting();
        updateCursor();
    }
}
