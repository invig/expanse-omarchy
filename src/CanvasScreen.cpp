#define WLR_USE_UNSTABLE

#include "Canvas.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <wayland-server-core.h>

#include <cmath>

// Following you between screens, as on the Mac: the canvas keeps a home
// layout at the sizes you chose. On a smaller screen windows shrink to fit and
// the layout tightens (each window keeps its gap to its neighbours); back on
// the big one everything returns exactly.
namespace Expanse {

    using Desktop::View::IGeometric;

    constexpr double FIT_MARGIN     = 16.0; // around a window fitted to the screen as you go to it
    constexpr int    RELAYOUT_DELAY = 1000; // ms for a screen change to settle

    static CBox goalBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)};
    }

    CCanvas::STracked* CCanvas::tracked(const PHLWINDOW& window) {
        for (auto& entry : m_originals)
            if (entry.window.lock() == window)
                return &entry;
        return nullptr;
    }

    bool CCanvas::isDerived() const {
        // True while the canvas is a tightened version of the home layout:
        // some window is smaller than its chosen size because this screen is.
        for (const auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW)
                continue;
            const auto SIZE = goalBox(WINDOW).size();
            if (SIZE.x < entry.preferred.x - 1 || SIZE.y < entry.preferred.y - 1)
                return true;
        }
        return false;
    }

    void CCanvas::noteSizes() {
        // A size the canvas didn't make is the size you chose.
        for (auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW)
                continue;
            const auto SIZE = goalBox(WINDOW).size();
            const bool OURS = entry.lastSized && std::abs(entry.lastSized->x - SIZE.x) < 2 && std::abs(entry.lastSized->y - SIZE.y) < 2;
            if (!OURS && (std::abs(SIZE.x - entry.preferred.x) >= 2 || std::abs(SIZE.y - entry.preferred.y) >= 2)) {
                entry.preferred = SIZE;
                entry.lastSized.reset();
            }
        }
    }

    void CCanvas::updateHome() {
        // While no window is shrunk for a smaller screen, the canvas layout is the home layout.
        if (isDerived())
            return;
        for (auto& entry : m_originals)
            if (const auto WINDOW = entry.window.lock(); WINDOW && WINDOW->m_workspace)
                entry.home = goalBox(WINDOW).pos() + scrolledFor(WINDOW->m_workspace);
    }

    void CCanvas::noteUserMove(const PHLWINDOW& window, const Vector2D& delta) {
        // On a smaller screen the home layout moves by the same amount, so the
        // move survives going back to the big screen.
        if (!isDerived())
            return;
        if (auto* entry = tracked(window))
            entry->home = entry->home + delta;
    }

    void CCanvas::resizeWindow(const PHLWINDOW& window, const Vector2D& size) {
        const auto TARGET = window->layoutTarget();
        if (!TARGET || !TARGET->floating())
            return;
        noteOriginal(window);
        CBox       layout = TARGET->position();
        const CBox NOW    = goalBox(window);
        layout.w += size.x - NOW.w;
        layout.h += size.y - NOW.h;
        g_layoutManager->setTargetGeom(layout, TARGET);
        TARGET->warpPositionSize();
        if (auto* entry = tracked(window))
            entry->lastSized = goalBox(window).size();
    }

    void CCanvas::fitToScreen(const PHLWINDOW& window) {
        // Shrink a window that doesn't fit the screen; grow it back toward the
        // size you chose. It keeps its centre.
        noteOriginal(window);
        noteSizes();
        const auto* ENTRY = tracked(window);
        if (!ENTRY)
            return;
        const CBox     AREA = usableArea();
        const Vector2D SIZE = {std::round(std::min(ENTRY->preferred.x, AREA.w - FIT_MARGIN * 2)), std::round(std::min(ENTRY->preferred.y, AREA.h - FIT_MARGIN * 2))};
        const CBox     NOW  = goalBox(window);
        if (std::abs(SIZE.x - NOW.w) <= 1 && std::abs(SIZE.y - NOW.h) <= 1)
            return;
        resizeWindow(window, SIZE);
        moveWindow(window, (NOW.middle() - SIZE / 2.0).round());
    }

    void CCanvas::fitLayoutToScreen(double margin, bool tidy) {
        // Sizes: what you chose, capped at the screen. Positions: the home
        // layout, tightened by however much windows shrank, so every gap keeps
        // its size; on the big screen that's the home layout exactly. With
        // `tidy`, the home layout itself is cleaned up first (overlaps apart,
        // groups pulled to the standard gap).
        const auto WINDOWS = canvasWindows();
        if (WINDOWS.empty())
            return;
        for (const auto& window : WINDOWS)
            noteOriginal(window);

        const Vector2D    SCROLL  = scrolledFor(workspace());
        const CBox        AREA    = usableArea();
        const Vector2D    MAXSIZE = {AREA.w - margin * 2, AREA.h - margin * 2};
        std::vector<CBox> home;
        for (const auto& window : WINDOWS) {
            const auto* ENTRY = tracked(window);
            home.push_back({ENTRY->home, ENTRY->preferred});
        }

        if (tidy) {
            Arrange::separate(home, Arrange::GAP);
            Arrange::gravity(home, Arrange::clusters(home, Arrange::CLUSTER_REACH), Arrange::GAP);
            Arrange::separate(home, Arrange::GAP);
            for (size_t i = 0; i < WINDOWS.size(); ++i)
                tracked(WINDOWS[i])->home = home[i].pos().round();
        }

        std::vector<Vector2D> sizes;
        for (const auto& window : WINDOWS) {
            const auto*    ENTRY = tracked(window);
            const Vector2D SIZE  = {std::round(std::min(ENTRY->preferred.x, MAXSIZE.x)), std::round(std::min(ENTRY->preferred.y, MAXSIZE.y))};
            const CBox     NOW   = goalBox(window);
            if (std::abs(SIZE.x - NOW.w) > 1 || std::abs(SIZE.y - NOW.h) > 1)
                resizeWindow(window, SIZE);
            sizes.push_back(goalBox(window).size());
        }

        const auto        ORIGINS = Arrange::derive(home, sizes);
        std::vector<CBox> frames;
        for (size_t i = 0; i < WINDOWS.size(); ++i)
            frames.push_back({ORIGINS[i], sizes[i]});
        // Only overlaps this screen caused, pushed apart to the standard gap.
        // Windows you have overlapping in the home layout stay that way.
        Arrange::separate(frames, Arrange::GAP, true, Arrange::overlappingPairs(home));

        for (size_t i = 0; i < WINDOWS.size(); ++i)
            moveWindow(WINDOWS[i], frames[i].pos().round() - SCROLL);
    }

    void CCanvas::simulateScreen(std::optional<Vector2D> size) {
        m_screenOverride = size;
        relayout();
    }

    void CCanvas::onScreenChanged() {
        // Wait for the change to settle, then fit the canvas to the screen.
        if (m_paused || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!m_relayoutTimer)
            m_relayoutTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void* data) {
                    sc<CCanvas*>(data)->relayout();
                    return 0;
                },
                this);
        wl_event_source_timer_update(m_relayoutTimer, RELAYOUT_DELAY);
    }

    void CCanvas::relayout() {
        if (m_paused)
            return;
        if (live()) {
            onScreenChanged(); // once the canvas is back at 100%
            return;
        }
        const auto WORKSPACE = workspace();
        if (!WORKSPACE)
            return;

        for (const auto& window : canvasWindows())
            noteOriginal(window);
        noteSizes();
        updateHome();

        const CBox SCREEN = usableArea();
        if (m_lastScreen && (std::abs(m_lastScreen->w - SCREEN.w) > 0.5 || std::abs(m_lastScreen->h - SCREEN.h) > 0.5)) {
            // Whatever was in the middle of the old screen stays in the middle of the new one.
            m_scrolled[WORKSPACE->m_name] = scrolledFor(WORKSPACE) - (SCREEN.middle() - m_lastScreen->middle()).round();
        }
        m_lastScreen = SCREEN;

        fitLayoutToScreen(0, false);
        damage();
        scheduleSave();
    }
}
