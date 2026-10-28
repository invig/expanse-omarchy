#define WLR_USE_UNSTABLE

#include "Canvas.hpp"
#include "Log.hpp"
#include "Mode.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <cmath>

// Following you between screens, as on the Mac: the canvas keeps a home
// layout at the sizes you chose. On a smaller screen windows shrink to fit and
// the layout tightens (each window keeps its gap to its neighbours); back on
// the big one everything returns exactly.
namespace Expanse {

    using Desktop::View::IGeometric;

    // The gap kept around a window that would otherwise fill the screen. Every
    // fit uses the same one, or a window fitted after a screen change would
    // shrink (and shift) the first time you go to it.
    constexpr double SCREEN_MARGIN  = 16.0;
    constexpr int    RELAYOUT_DELAY = 1000; // ms for a screen change to settle
    // How far short of the size asked for a window can end up because its app
    // rounds sizes (a terminal to whole character cells): that's still the
    // chosen size, not one shrunk for a smaller screen, which takes far more.
    constexpr double ROUNDING       = 32.0;

    static CBox goalBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)};
    }

    // Full screen (or maximised), a window's box is the screen's, not a spot
    // or size on the canvas: it keeps the ones it had.
    static bool fullscreen(const PHLWINDOW& window) {
        return Fullscreen::controller()->isFullscreen(window);
    }

    CCanvas::STracked* CCanvas::tracked(const PHLWINDOW& window) {
        for (auto& entry : m_originals)
            if (entry.window.lock() == window)
                return &entry;
        return nullptr;
    }

    bool CCanvas::isDerived(const PHLWORKSPACE& workspace) const {
        // True while the workspace's canvas is a tightened version of its home
        // layout: some window is smaller than its chosen size because this
        // screen is.
        for (const auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW || WINDOW->m_workspace != workspace || fullscreen(WINDOW) || sizesItself(WINDOW))
                continue;
            const auto SIZE = goalBox(WINDOW).size();
            if (SIZE.x < entry.preferred.x - ROUNDING || SIZE.y < entry.preferred.y - ROUNDING)
                return true;
        }
        return false;
    }

    void CCanvas::noteSizes() {
        // A size the canvas didn't make is the size you chose.
        for (auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW || fullscreen(WINDOW))
                continue;
            const auto SIZE = goalBox(WINDOW).size();
            // The size the canvas asked for, as near as its app goes (it gets
            // there after the ask, rounded).
            const bool OURS = entry.lastSized && std::abs(entry.lastSized->x - SIZE.x) <= ROUNDING && std::abs(entry.lastSized->y - SIZE.y) <= ROUNDING;
            if (!OURS && (std::abs(SIZE.x - entry.preferred.x) >= 2 || std::abs(SIZE.y - entry.preferred.y) >= 2)) {
                entry.preferred = SIZE;
                entry.lastSized.reset();
            } else if (OURS && std::abs(entry.lastSized->x - entry.preferred.x) < 1 && std::abs(entry.lastSized->y - entry.preferred.y) < 1) {
                // Asked for the size you chose and the app came as close as it
                // goes: that's the chosen size from now on, rather than asking
                // again at every fit.
                entry.preferred = SIZE;
                entry.lastSized = SIZE;
            }
        }
    }

    void CCanvas::updateHome() {
        // While no window on a workspace is shrunk for a smaller screen, its
        // canvas layout is its home layout. Not on a workspace whose screen
        // changed since it was fitted (moved to another monitor while out of
        // sight, say): its windows are where Hyprland put them until then.
        std::vector<std::pair<PHLWORKSPACE, bool>> derived;
        const auto DERIVED = [&](const PHLWORKSPACE& workspace) {
            for (const auto& [ws, is] : derived)
                if (ws == workspace)
                    return is;
            const auto LAST    = m_lastScreens.find(workspace->m_name);
            const auto MONITOR = workspace->m_monitor.lock();
            const bool WAITING = LAST != m_lastScreens.end() && MONITOR &&
                (LAST->second.pos() != usableAreaOf(MONITOR).pos() || LAST->second.size() != usableAreaOf(MONITOR).size());
            derived.emplace_back(workspace, WAITING || isDerived(workspace));
            return derived.back().second;
        };
        for (auto& entry : m_originals)
            if (const auto WINDOW = entry.window.lock(); WINDOW && WINDOW->m_workspace && !fullscreen(WINDOW) && !DERIVED(WINDOW->m_workspace)) {
                entry.home      = goalBox(WINDOW).pos() + scrolledFor(WINDOW->m_workspace);
                entry.homeKnown = true;
            }
    }

    void CCanvas::noteUserMove(const PHLWINDOW& window) {
        // On the big screen its new spot is its home spot, straight away:
        // anything that fits the layout from home spots before the next save
        // (Tidy Up, a window coming back) would otherwise put it back. On a
        // smaller screen its home spot keeps the gaps it now has to its
        // neighbours (see Arrange::homeSpot), so the move survives going back
        // to the big screen.
        auto* entry = tracked(window);
        if (!entry)
            return;
        // Where you put it is where it goes: no push left for the next fit to take back.
        entry->fitPush = {};
        if (!isDerived(window->m_workspace)) {
            entry->home      = goalBox(window).pos() + scrolledFor(window->m_workspace);
            entry->homeKnown = true;
            return;
        }
        auto WINDOWS = canvasWindows();
        std::erase_if(WINDOWS, fullscreen);
        const auto IT = std::ranges::find(WINDOWS, window);
        if (IT == WINDOWS.end())
            return;
        const auto        HERE = fittedFrames(WINDOWS, canvasSpots(WINDOWS));
        std::vector<CBox> home;
        for (size_t j = 0; j < WINDOWS.size(); ++j) {
            const auto* OTHER = tracked(WINDOWS[j]);
            home.push_back({OTHER && OTHER->homeKnown ? OTHER->home : HERE[j].pos(), OTHER ? homeSize(*OTHER, WINDOWS[j]) : HERE[j].size()});
        }
        entry           = tracked(window);
        entry->home      = Arrange::homeSpot(IT - WINDOWS.begin(), HERE, home);
        entry->homeKnown = true;
    }

    Vector2D CCanvas::homeSize(const STracked& entry, const PHLWINDOW& window) const {
        // The size you chose, or for a VM window (which picks its own size)
        // the size it is.
        return sizesItself(window) || entry.preferred.x <= 0 ? goalBox(window).size() : entry.preferred;
    }

    Vector2D CCanvas::fittedSize(const STracked& entry, const PHLWINDOW& window, const Vector2D& screen) const {
        // What fitting gives it on a screen of this size: the size you chose,
        // capped at the screen.
        if (sizesItself(window))
            return goalBox(window).size();
        const Vector2D CHOSEN = entry.preferred.x > 0 ? entry.preferred : goalBox(window).size();
        return {std::min(CHOSEN.x, std::round(screen.x - SCREEN_MARGIN * 2)), std::min(CHOSEN.y, std::round(screen.y - SCREEN_MARGIN * 2))};
    }

    CCanvas::Spots CCanvas::canvasSpots(const std::vector<PHLWINDOW>& windows) const {
        Spots spots;
        for (const auto& window : windows)
            spots.emplace_back(window, goalBox(window).pos() + scrolledFor(window->m_workspace));
        return spots;
    }

    std::vector<CBox> CCanvas::fittedFrames(const std::vector<PHLWINDOW>& windows, const Spots& spots) {
        // Where the last fit had each window, before pushing it clear of
        // overlaps, at the size that fit gave it.
        const auto        WORKSPACE = workspace();
        const auto        FITTED    = WORKSPACE && m_fitted.contains(WORKSPACE->m_name) ? std::optional{m_fitted[WORKSPACE->m_name]} : std::nullopt;
        std::vector<CBox> frames;
        for (size_t i = 0; i < windows.size(); ++i) {
            const auto* ENTRY = tracked(windows[i]);
            const auto  SPOT  = spots[i].second - (ENTRY ? ENTRY->fitPush : Vector2D{});
            frames.push_back({SPOT, ENTRY && FITTED ? fittedSize(*ENTRY, windows[i], *FITTED) : goalBox(windows[i]).size()});
        }
        return frames;
    }

    void CCanvas::rehomeStrays(const std::vector<PHLWINDOW>& windows, const Spots& spots) {
        // Windows whose home spot doesn't explain where they are get one from
        // their neighbours here (see Arrange::rehome).
        const auto WORKSPACE = workspace();
        const auto FITTED    = WORKSPACE && m_fitted.contains(WORKSPACE->m_name) ? std::optional{m_fitted[WORKSPACE->m_name]} : std::nullopt;
        const auto HERE      = fittedFrames(windows, spots);
        std::vector<std::optional<Vector2D>> homes;
        std::vector<Vector2D>                homeSizes, sizes;
        for (size_t i = 0; i < windows.size(); ++i) {
            const auto* ENTRY = tracked(windows[i]);
            homes.push_back(ENTRY->homeKnown ? std::optional{ENTRY->home} : std::nullopt);
            homeSizes.push_back(homeSize(*ENTRY, windows[i]));
            sizes.push_back(FITTED ? fittedSize(*ENTRY, windows[i], *FITTED) : goalBox(windows[i]).size());
        }
        std::string names;
        for (const size_t I : Arrange::rehome(HERE, homes, homeSizes, sizes)) {
            auto* entry      = tracked(windows[I]);
            entry->home      = *homes[I];
            entry->homeKnown = true;
            names += (names.empty() ? "" : ", ") + logName(windows[I]);
        }
        if (!names.empty())
            Log::info("re-homed: {}", names);
    }

    void CCanvas::resizeWindow(const PHLWINDOW& window, const Vector2D& size) {
        const auto TARGET = window->layoutTarget();
        if (!TARGET || !TARGET->floating() || sizesItself(window))
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
        if (sizesItself(window))
            return;
        noteOriginal(window);
        noteSizes();
        const auto* ENTRY = tracked(window);
        if (!ENTRY)
            return;
        const CBox     AREA = usableArea();
        const Vector2D SIZE = {std::round(std::min(ENTRY->preferred.x, AREA.w - SCREEN_MARGIN * 2)), std::round(std::min(ENTRY->preferred.y, AREA.h - SCREEN_MARGIN * 2))};
        const CBox     NOW  = goalBox(window);
        if (std::abs(SIZE.x - NOW.w) <= 1 && std::abs(SIZE.y - NOW.h) <= 1)
            return;
        resizeWindow(window, SIZE);
        moveWindow(window, (NOW.middle() - SIZE / 2.0).round());
    }

    void CCanvas::fitLayoutToScreen(bool tidy, const std::optional<std::vector<PHLWINDOW>>& rederiving, const Spots* spots) {
        // Sizes: what you chose, capped at the screen. Positions: the home
        // layout, tightened by however much windows shrank, so every gap keeps
        // its size; on the big screen that's the home layout exactly. With
        // `tidy`, the home layout itself is cleaned up first (overlaps apart,
        // groups pulled to the standard gap).
        // `rederiving`: the windows placed from their home spots; all of them
        // without it. The rest keep where you see them (`spots`, or where they
        // are), moved only as far as the screen change since the last fit
        // moves their spot in the derived layout. Re-deriving everything would
        // also undo whatever the layout picked up since then (windows opening
        // and closing change the derived layout), and every window would jump.
        auto WINDOWS = canvasWindows();
        std::erase_if(WINDOWS, fullscreen);
        const auto WORKSPACE = workspace();
        if (WINDOWS.empty() || !WORKSPACE)
            return;
        for (const auto& window : WINDOWS)
            noteOriginal(window);

        Spots here = canvasSpots(WINDOWS);
        if (spots)
            for (auto& [window, spot] : here)
                for (const auto& [noted, at] : *spots)
                    if (noted == window)
                        spot = at;
        rehomeStrays(WINDOWS, here);

        const Vector2D    SCROLL  = scrolledFor(WORKSPACE);
        const CBox        AREA    = usableArea();
        const Vector2D    MAXSIZE = {AREA.w - SCREEN_MARGIN * 2, AREA.h - SCREEN_MARGIN * 2};
        std::vector<CBox> home;
        for (const auto& window : WINDOWS) {
            const auto* ENTRY = tracked(window);
            home.push_back({ENTRY->home, homeSize(*ENTRY, window)});
        }

        if (tidy) {
            Arrange::tidyUp(home);
            for (size_t i = 0; i < WINDOWS.size(); ++i) {
                tracked(WINDOWS[i])->home      = home[i].pos().round();
                tracked(WINDOWS[i])->homeKnown = true;
            }
        }

        std::vector<Vector2D> sizes, actual;
        for (size_t i = 0; i < WINDOWS.size(); ++i) {
            const auto&    window = WINDOWS[i];
            const auto*    ENTRY  = tracked(window);
            const Vector2D SIZE   = {std::round(std::min(ENTRY->preferred.x, MAXSIZE.x)), std::round(std::min(ENTRY->preferred.y, MAXSIZE.y))};
            const CBox     NOW    = goalBox(window);
            if (std::abs(SIZE.x - NOW.w) > 1 || std::abs(SIZE.y - NOW.h) > 1)
                resizeWindow(window, SIZE);
            // A little off its home size, when that fits the screen, is the
            // app rounding (a terminal to its cells): counted as home, or the
            // windows beyond it would creep every time the screen changes.
            const Vector2D GOT     = goalBox(window).size();
            const bool     ROUNDED = home[i].w <= MAXSIZE.x && home[i].h <= MAXSIZE.y && std::abs(GOT.x - home[i].w) <= ROUNDING && std::abs(GOT.y - home[i].h) <= ROUNDING;
            sizes.push_back(ROUNDED ? home[i].size() : GOT);
            actual.push_back(GOT);
        }

        auto       origins = Arrange::derive(home, sizes);
        const auto FITTED  = m_fitted.contains(WORKSPACE->m_name) ? std::optional{m_fitted[WORKSPACE->m_name]} : std::nullopt;
        if (rederiving && !tidy && FITTED) {
            // Worked out from the screen sizes rather than measured, so a
            // round trip between two screens comes back exactly. Last fit's
            // push apart is taken back first: on this screen there may be no
            // overlap to push.
            std::vector<Vector2D> was, now;
            for (const auto& window : WINDOWS) {
                was.push_back(fittedSize(*tracked(window), window, *FITTED));
                now.push_back(fittedSize(*tracked(window), window, AREA.size()));
            }
            const auto BEFORE = Arrange::derive(home, was);
            const auto AFTER  = Arrange::derive(home, now);
            for (size_t i = 0; i < WINDOWS.size(); ++i)
                if (!std::ranges::contains(*rederiving, WINDOWS[i]))
                    origins[i] = here[i].second - tracked(WINDOWS[i])->fitPush + AFTER[i] - BEFORE[i];
        }
        std::vector<CBox> frames;
        for (size_t i = 0; i < WINDOWS.size(); ++i)
            frames.push_back({origins[i], actual[i]});
        // Only overlaps this screen caused, pushed apart to the standard gap.
        // Windows you have overlapping in the home layout stay that way.
        Arrange::separate(frames, Arrange::GAP, true, Arrange::overlappingPairs(home));
        for (size_t i = 0; i < WINDOWS.size(); ++i)
            tracked(WINDOWS[i])->fitPush = frames[i].pos() - origins[i];
        m_fitted[WORKSPACE->m_name] = AREA.size();

        int moved = 0;
        for (size_t i = 0; i < WINDOWS.size(); ++i) {
            const Vector2D TO = frames[i].pos().round() - SCROLL;
            if (TO.distance(goalBox(WINDOWS[i]).pos()) >= 1)
                ++moved;
            moveWindow(WINDOWS[i], TO);
        }
        Log::info("layout {} on {} for {:.0f}x{:.0f}: {} of {} windows moved{}", tidy ? "tidied" : "fitted", WORKSPACE->m_name, AREA.w, AREA.h, moved, WINDOWS.size(),
                  isDerived(WORKSPACE) ? ", derived" : "");
    }

    void CCanvas::simulateScreen(std::optional<Vector2D> size) {
        // The screen it's laid out for now, if it hasn't been noted yet.
        if (const auto WORKSPACE = workspace(); WORKSPACE && !m_lastScreens.contains(WORKSPACE->m_name))
            noteScreen(WORKSPACE, usableArea());
        if (!m_centreAfterResize.lock())
            m_centreAfterResize = Desktop::focusState()->window();
        m_screenOverride = size;
        relayout();
        m_centreAfterResize.reset();
    }

    void CCanvas::onScreenChanged() {
        // Wait for the change to settle, then fit the canvas to the screen.
        // Meanwhile focus moving about isn't you going to a window (as on the
        // Mac, where window moves are ignored while macOS reshuffles them).
        m_settledAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(RELAYOUT_DELAY + 500);
        if (m_paused || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        // The window you're in, before anything moves: once the layout fits
        // the new screen, it's in the middle.
        if (!m_centreAfterResize.lock())
            m_centreAfterResize = Desktop::focusState()->window();
        if (!m_relayoutTimer)
            m_relayoutTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void* data) {
                    sc<CCanvas*>(data)->relayoutAll();
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
        followWorkspaces();
        const auto WORKSPACE = workspace();
        if (!WORKSPACE || !Mode::isExpanse(WORKSPACE))
            return;

        const CBox     SCREEN = usableArea();
        const auto     LAST   = m_lastScreens.contains(WORKSPACE->m_name) ? std::optional{m_lastScreens[WORKSPACE->m_name]} : std::nullopt;
        // On another monitor (its own went away, say), not this one changed:
        // its windows keep their places on the screen, as Hyprland brought them.
        const auto     WAS     = m_lastMonitors.find(WORKSPACE->m_name);
        const auto     MONITOR = WORKSPACE->m_monitor.lock();
        const bool     ANOTHER = LAST && MONITOR && WAS != m_lastMonitors.end() && WAS->second != MONITOR->m_name;
        const Vector2D MOVED  = LAST ? SCREEN.pos() - LAST->pos() : Vector2D{};
        const Vector2D SCROLL = scrolledFor(WORKSPACE);
        if (MOVED != Vector2D{}) {
            // On another monitor, or this one moved. Hyprland brings floating
            // windows along when it moves a workspace off a monitor that went
            // away, but not back again: see how far they came, so their spots
            // on the canvas read as they were before anything else.
            m_scrolled[WORKSPACE->m_name] = SCROLL - cameWith(WORKSPACE, SCROLL, MOVED);
        }
        // The size it was fitted to: noted with the layout, or (a layout from
        // before that was kept) the screen it was laid out for.
        if (LAST && !m_fitted.contains(WORKSPACE->m_name))
            m_fitted[WORKSPACE->m_name] = LAST->size();
        noteScreen(WORKSPACE, SCREEN);

        for (const auto& window : canvasWindows())
            noteOriginal(window);
        noteSizes();
        updateHome();
        // Where each window is on the canvas, before the view follows the
        // monitor (and keeps its middle): fitting moves them from there.
        auto WINDOWS = canvasWindows();
        std::erase_if(WINDOWS, fullscreen);
        const Spots SPOTS = canvasSpots(WINDOWS);

        // The canvas follows its monitor: the windows keep their places on the screen.
        if (MOVED != Vector2D{})
            m_scrolled[WORKSPACE->m_name] = SCROLL - MOVED;

        const bool RESIZED = LAST && !ANOTHER && (std::abs(LAST->w - SCREEN.w) > 0.5 || std::abs(LAST->h - SCREEN.h) > 0.5);
        Log::info("screen change for workspace {}: {} {:.0f}x{:.0f}{}", WORKSPACE->m_name, ANOTHER ? "now on monitor" : "monitor", SCREEN.w, SCREEN.h,
                  LAST ? std::format(", was {:.0f}x{:.0f}", LAST->w, LAST->h) : "");
        if (RESIZED) {
            // Whatever was in the middle of the old screen stays in the middle of the new one.
            m_scrolled[WORKSPACE->m_name] = scrolledFor(WORKSPACE) - ((SCREEN.size() - LAST->size()) / 2.0).round();
        }

        fitLayoutToScreen(false, std::vector<PHLWINDOW>{}, &SPOTS);
        if (RESIZED)
            centreAfterResize(WORKSPACE);
        damage();
        scheduleSave();
    }

    void CCanvas::noteScreen(const PHLWORKSPACE& workspace, const CBox& screen) {
        m_lastScreens[workspace->m_name] = screen;
        if (const auto MONITOR = workspace->m_monitor.lock())
            m_lastMonitors[workspace->m_name] = MONITOR->m_name;
    }

    void CCanvas::centreAfterResize(const PHLWORKSPACE& workspace) {
        // The window you were in goes in the middle, as on the Mac: fitting
        // moves windows (the layout tightens on a smaller screen), so the old
        // middle can be somewhere else now.
        const auto WINDOW = m_centreAfterResize.lock();
        if (!WINDOW || WINDOW->m_workspace != workspace || !isCanvasWindow(WINDOW) || fullscreen(WINDOW))
            return;
        const Vector2D DELTA = (goalBox(WINDOW).middle() - usableArea().middle()).round();
        if (DELTA.size() < 0.5)
            return;
        // The view moves: every window by the same amount, its canvas spot the same.
        Log::info("screen change: centred on {}", logName(WINDOW));
        m_viewPlaced                  = false;
        m_scrolled[workspace->m_name] = scrolledFor(workspace) + DELTA;
        for (const auto& window : canvasWindows())
            if (!fullscreen(window))
                moveWindow(window, goalBox(window).pos() - DELTA);
    }

    Vector2D CCanvas::cameWith(const PHLWORKSPACE& workspace, const Vector2D& scroll, const Vector2D& moved) const {
        // How far the workspace's windows moved since their spots were noted:
        // the middle value, so a window you moved yourself doesn't count. With
        // nothing to go by, as far as the monitor did.
        std::vector<double> xs, ys;
        for (const auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW || WINDOW->m_workspace != workspace || fullscreen(WINDOW))
                continue;
            const Vector2D D = goalBox(WINDOW).pos() + scroll - entry.home;
            xs.push_back(D.x);
            ys.push_back(D.y);
        }
        if (xs.empty())
            return moved;
        std::ranges::nth_element(xs, xs.begin() + xs.size() / 2);
        std::ranges::nth_element(ys, ys.begin() + ys.size() / 2);
        return {xs[xs.size() / 2], ys[ys.size() / 2]};
    }

    void CCanvas::relayoutOn(const PHLMONITOR& monitor) {
        // Only a workspace whose screen changed: fitting also shrinks windows
        // bigger than the screen. One never laid out just notes its screen.
        SOnMonitor here{*this, monitor};
        const auto WORKSPACE = workspace();
        const CBox SCREEN    = usableArea();
        if (WORKSPACE && Mode::isExpanse(WORKSPACE)) {
            const auto IT = m_lastScreens.find(WORKSPACE->m_name);
            if (IT == m_lastScreens.end())
                noteScreen(WORKSPACE, SCREEN);
            else if (IT->second.pos() != SCREEN.pos() || IT->second.size() != SCREEN.size())
                relayout();
        }
    }

    void CCanvas::relayoutAll() {
        if (live()) {
            onScreenChanged(); // once the canvas is back at 100%
            return;
        }
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && monitor->m_enabled && monitor->m_activeWorkspace)
                relayoutOn(monitor);
        m_centreAfterResize.reset();
    }
}
