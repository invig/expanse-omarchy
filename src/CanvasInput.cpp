#define WLR_USE_UNSTABLE

#include "Canvas.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <format>

// Pointer, keyboard, scroll and pinch on the canvas, as on the Mac.
namespace Expanse {

    constexpr double DRAG_SLOP   = 3.0; // screen px a press may move on the canvas before it's a drag
    constexpr double CLICK_SLOP  = 6.0; // at 100%, before a click on a window is a drag
    constexpr double ALT_PINCH   = 0.88; // ALT + pinching in this far anywhere opens the canvas
    constexpr double WHEEL_PAN   = 4.0;  // per unit of wheel delta (15 a notch)
    constexpr double WHEEL_ZOOM  = 0.008;
    constexpr double FINGER_ZOOM = 0.006;

    static uint32_t mods() {
        return g_pInputManager->getModsFromAllKBs();
    }

    static Vector2D pointer() {
        return g_pInputManager->getMouseCoordsInternal();
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

    // Over the empty desktop: nothing, or only the wallpaper and other
    // background / bottom layers, under the pointer.
    static bool pointerOnDesktop() {
        const auto SURFACE = g_pSeatManager->m_state.pointerFocus.lock();
        if (!SURFACE)
            return true;
        const auto WLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        const auto VIEW      = WLSURFACE ? WLSURFACE->view() : nullptr;
        if (!VIEW)
            return true;
        if (VIEW->type() != Desktop::View::VIEW_TYPE_LAYER_SURFACE)
            return false;
        const auto LAYER = Desktop::View::CLayerSurface::fromView(VIEW);
        return LAYER && LAYER->m_layer <= 1; // background, bottom
    }

    bool CCanvas::onMouseMove(const Vector2D& pos) {
        if (!live())
            return false;

        if (m_drag.kind != SDrag::DRAG_NONE) {
            dragMove(pos);
            return true;
        }

        const auto HOVER = windowAt(pos);
        if (HOVER != m_hover.lock()) {
            m_hover = HOVER;
            damage();
        }

        const auto BAR   = spacesBar();
        const auto TILE  = BAR ? BAR->tileAt(pos) : std::nullopt;
        const auto SPACE = TILE ? BAR->tiles[*TILE].workspace.lock() : nullptr;
        if (SPACE != m_hoverSpace.lock()) {
            m_hoverSpace = SPACE;
            damage();
        }

        // Swallowed so apps don't see a pointer over their real, off-view
        // positions and focus doesn't follow the mouse.
        return true;
    }

    void CCanvas::dragMove(const Vector2D& pos) {
        switch (m_drag.kind) {
            case SDrag::DRAG_NONE: break;
            case SDrag::DRAG_PAN:
                m_drag.moved = true;
                pan(pos - m_drag.last);
                break;
            case SDrag::DRAG_WINDOW: {
                if (!m_drag.moved && pos.distance(m_drag.start) < DRAG_SLOP)
                    return;
                m_drag.moved = true;

                // The group's bounds snap as one block against everything else.
                const Vector2D    D = (pos - m_drag.start) / scale();
                std::vector<CBox> others;
                for (const auto& window : canvasWindows())
                    if (std::ranges::none_of(m_drag.starts, [&](const auto& start) { return start.first.lock() == window; }))
                        others.push_back({window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL), window->size(Desktop::View::IGeometric::GEOMETRIC_GOAL)});

                CBox bounds = m_drag.starts.front().second;
                for (const auto& [ref, box] : m_drag.starts) {
                    const double X0 = std::min(bounds.x, box.x), Y0 = std::min(bounds.y, box.y);
                    bounds          = {X0, Y0, std::max(bounds.x + bounds.w, box.x + box.w) - X0, std::max(bounds.y + bounds.h, box.y + box.h) - Y0};
                }
                const CBox PROPOSED = bounds.copy().translate(D);

                // SUPER turns snapping off, as ⌘ does on the Mac.
                Vector2D shift = D;
                m_guides.clear();
                if (!(mods() & HL_MODIFIER_META)) {
                    const auto SNAPPED = Arrange::snap(PROPOSED, others, scale(), viewCentre().pos());
                    shift              = D + (SNAPPED.box.pos() - PROPOSED.pos());
                    m_guides           = SNAPPED.guides;
                }

                for (const auto& [ref, box] : m_drag.starts)
                    if (const auto WINDOW = ref.lock())
                        moveWindow(WINDOW, box.pos() + shift);
                break;
            }
            case SDrag::DRAG_MINIMAP:
                if (const auto MAP = minimap())
                    centreOn(MAP->toCanvas(pos));
                break;
            case SDrag::DRAG_MARQUEE: {
                m_drag.moved   = true;
                m_drag.last    = pos;
                const auto BOX = *marquee();
                m_selected     = m_drag.base;
                for (const auto& window : canvasWindows())
                    if (!screenBox(window).intersection(BOX).empty() && !isSelected(window))
                        m_selected.emplace_back(window);
                damage();
                break;
            }
        }
        m_drag.last = pos;
    }

    bool CCanvas::onMouseButton(uint32_t button, bool pressed) {
        if (m_paused)
            return false;

        if (!live()) {
            // At 100%, clicking a window that isn't fully on screen centres it.
            // The click itself goes to the app; a drag (selecting, moving the
            // window) doesn't count.
            if (button != BTN_LEFT)
                return false;

            const auto POS = pointer();
            if (pressed)
                m_press = {POS, windowUnderPointer()};
            else if (const auto WINDOW = m_press.window.lock(); WINDOW && POS.distance(m_press.pos) <= CLICK_SLOP && !fullyVisible(WINDOW))
                glideLater(WINDOW, false);
            return false;
        }

        if (m_state == STATE_GLIDING)
            return true;

        const auto POS = pointer();

        if (!pressed) {
            const auto DRAG = m_drag;
            m_drag          = {};
            m_guides.clear();
            damage();
            if (DRAG.kind == SDrag::DRAG_WINDOW && DRAG.moved) {
                for (const auto& [ref, box] : DRAG.starts)
                    if (const auto WINDOW = ref.lock())
                        noteUserMove(WINDOW, WINDOW->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) - box.pos());
                scheduleSave();
            }
            // A click (no drag) on a window dives into it.
            if (DRAG.kind == SDrag::DRAG_WINDOW && !DRAG.moved)
                if (const auto WINDOW = DRAG.window.lock())
                    glideTo(WINDOW);
            return true;
        }

        if (button == BTN_RIGHT) {
            close();
            return true;
        }

        if (button == BTN_MIDDLE || (button == BTN_LEFT && m_spaceHeld)) {
            m_drag = {.kind = SDrag::DRAG_PAN, .start = POS, .last = POS};
            return true;
        }

        if (button != BTN_LEFT)
            return true;

        // A press stops the camera where it is.
        if (m_animating)
            setCamera(m_camera);

        // On the workspaces bar: a tile goes to that workspace's canvas.
        if (const auto BAR = spacesBar(); BAR && BAR->box.containsPoint(POS)) {
            if (const auto TILE = BAR->tileAt(POS))
                switchTo(BAR->tiles[*TILE].workspace.lock());
            return true;
        }

        // On the minimap: the view centres there, and follows a drag.
        if (const auto MAP = minimap(); MAP && MAP->box.containsPoint(POS)) {
            m_drag = {.kind = SDrag::DRAG_MINIMAP, .start = POS, .last = POS};
            if (MAP->area.copy().expand(4).containsPoint(POS))
                centreOn(MAP->toCanvas(POS));
            return true;
        }

        const bool SHIFT  = mods() & HL_MODIFIER_SHIFT;
        const auto WINDOW = windowAt(POS);

        if (!WINDOW) {
            m_drag = {.kind = SDrag::DRAG_MARQUEE, .start = POS, .last = POS, .base = SHIFT ? m_selected : std::vector<PHLWINDOWREF>{}};
            if (!SHIFT)
                clearSelection();
            return true;
        }

        const bool GROUP = isSelected(WINDOW) && selection().size() > 1;

        if (showsClose(WINDOW) && closeButton(screenBox(WINDOW)).containsPoint(POS)) {
            closeWindows(GROUP ? selection() : std::vector<PHLWINDOW>{WINDOW});
            return true;
        }

        if (SHIFT) {
            // ⇧-click toggles the window in the selection.
            toggleSelected(WINDOW);
            return true;
        }

        if (!GROUP)
            clearSelection();
        const auto MOVING = GROUP ? selection() : std::vector<PHLWINDOW>{WINDOW};

        m_drag = {.kind = SDrag::DRAG_WINDOW, .start = POS, .last = POS, .window = WINDOW};
        for (const auto& window : MOVING) {
            m_drag.starts.emplace_back(window, CBox{window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL), window->size(Desktop::View::IGeometric::GEOMETRIC_GOAL)});
            raise(window);
        }
        damage();
        return true;
    }

    bool CCanvas::onAxis(const IPointer::SAxisEvent& event) {
        if (m_paused)
            return false;
        if (m_state == STATE_GLIDING)
            return true;

        if (!live()) {
            // Scrolling on the empty desktop starts moving around the canvas.
            if (!pointerOnDesktop())
                return false;
            openAt100();
            return live();
        }

        const bool WHEEL    = event.source == WL_POINTER_AXIS_SOURCE_WHEEL;
        const bool VERTICAL = event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;

        if (mods() & HL_MODIFIER_META) {
            // SUPER + scroll zooms around the pointer, as ⌘ + scroll on the Mac.
            if (VERTICAL)
                zoomBy(std::exp(-event.delta * (WHEEL ? WHEEL_ZOOM : FINGER_ZOOM)), pointer());
            return true;
        }

        const double AMOUNT = -event.delta * (WHEEL ? WHEEL_PAN : 1.0);
        pan(VERTICAL ? Vector2D{0.0, AMOUNT} : Vector2D{AMOUNT, 0.0});
        return true;
    }

    bool CCanvas::onPinchBegin() {
        m_pinchScale = 1.0;
        if (m_paused)
            return false;

        if (m_state == STATE_GLIDING)
            return true;

        if (live()) {
            m_pinching = true;
            return true;
        }

        if (pointerOnDesktop()) {
            openAt100();
            m_pinching = live();
            return m_pinching;
        }

        // ALT + pinching in anywhere opens the canvas, as ⌥ + pinch on the Mac.
        m_altPinch = mods() & HL_MODIFIER_ALT;
        return m_altPinch;
    }

    bool CCanvas::onPinchUpdate(double pinchScale) {
        if (m_altPinch) {
            if (pinchScale < ALT_PINCH) {
                m_altPinch = false;
                open();
            }
            return true;
        }

        if (!m_pinching)
            return m_state == STATE_GLIDING;

        zoomBy(pinchScale / m_pinchScale, pointer());
        m_pinchScale = pinchScale;
        return true;
    }

    bool CCanvas::onPinchEnd() {
        const bool OURS = m_pinching || m_altPinch;
        m_pinching      = false;
        m_altPinch      = false;
        return OURS;
    }

    bool CCanvas::onKey(uint32_t keycode, bool pressed) {
        if (!pressed) {
            if (keycode == KEY_SPACE)
                m_spaceHeld = false;
            const auto IT = std::ranges::find(m_swallowedKeys, keycode);
            if (IT == m_swallowedKeys.end())
                return false;
            m_swallowedKeys.erase(IT);
            return true;
        }

        if (!live())
            return false;

        const auto MODS = mods();
        if (m_state != STATE_GLIDING && handleCanvasKey(keycode, MODS)) {
            m_swallowedKeys.push_back(keycode);
            return true;
        }

        // Other chords stay keybinds: the toggle, hops (which chain mid-glide).
        if (MODS & (HL_MODIFIER_META | HL_MODIFIER_ALT | HL_MODIFIER_CTRL))
            return false;

        m_swallowedKeys.push_back(keycode);
        return true;
    }

    bool CCanvas::handleCanvasKey(uint32_t keycode, uint32_t mods) {
        const bool SUPER = mods & HL_MODIFIER_META;
        const bool PLAIN = !(mods & (HL_MODIFIER_META | HL_MODIFIER_ALT | HL_MODIFIER_CTRL));

        if (PLAIN) {
            const double SCALE = m_animating ? m_to.scale : m_camera.scale;
            switch (keycode) {
                case KEY_ESC:
                    // The selection first, then back to 100%.
                    if (!selection().empty())
                        clearSelection();
                    else
                        close();
                    return true;
                case KEY_ENTER:
                case KEY_KPENTER: {
                    // Into the highlighted window, or the top one.
                    auto window = m_hover.lock();
                    if (!window || !isCanvasWindow(window)) {
                        const auto WINDOWS = canvasWindows();
                        window             = WINDOWS.empty() ? nullptr : WINDOWS.back();
                    }
                    if (window)
                        glideTo(window, true);
                    else
                        close();
                    return true;
                }
                case KEY_LEFT: hop(DIRECTION_LEFT); return true;
                case KEY_RIGHT: hop(DIRECTION_RIGHT); return true;
                case KEY_UP: hop(DIRECTION_UP); return true;
                case KEY_DOWN: hop(DIRECTION_DOWN); return true;
                case KEY_SPACE: m_spaceHeld = true; return true;
                case KEY_0: fit(); return true;
                case KEY_1: zoomTo(1.0); return true;
                case KEY_MINUS: zoomTo(SCALE / 1.5); return true;
                case KEY_EQUAL: zoomTo(SCALE * 1.5); return true;
                case KEY_T: tidy(); return true;
                default: return false;
            }
        }

        // SUPER plays ⌘'s part.
        if (SUPER && keycode == KEY_A) {
            selectAll();
            return true;
        }
        if (SUPER && keycode == KEY_W) {
            // The selection if there is one, otherwise the window under the pointer.
            auto windows = selection();
            if (windows.empty())
                if (const auto HOVER = m_hover.lock(); HOVER && isCanvasWindow(HOVER))
                    windows = {HOVER};
            if (!windows.empty())
                closeWindows(windows);
            return true;
        }
        return false;
    }

    void CCanvas::onFocus(PHLWINDOW window, Desktop::eFocusReason reason) {
        // Keyboard focus (binds, Alt-Tab, a window closing) centres a window
        // that isn't fully on screen. Clicks are handled on release; focus
        // that follows the mouse never moves the canvas.
        if (m_paused || live() || reason == Desktop::FOCUS_REASON_FFM || reason == Desktop::FOCUS_REASON_CLICK)
            return;

        if (!isCanvasWindow(window) || fullyVisible(window))
            return;
        if (m_autoCentre) {
            glideLater(window, true);
            return;
        }
        // Auto-centre off means the canvas stays put, even for a window
        // entirely off screen; say where it went instead (once).
        if (screenBox(window).intersection(usableArea()).empty() && m_offscreenHint.lock() != window) {
            m_offscreenHint = window;
            HyprlandAPI::addNotification(PHANDLE, std::format("{} is off-screen · Alt+` to find it", appName(window)), CHyprColor{0.54F, 0.71F, 0.98F, 1.F}, 3000);
        }
    }
}
