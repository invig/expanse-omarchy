#define WLR_USE_UNSTABLE

#include "Canvas.hpp"
#include "HotCorner.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <ranges>
#include <set>

// Remembering the canvas, and switching in and out cleanly, as on the Mac.
namespace Expanse {

    using Desktop::View::IGeometric;

    constexpr size_t SAVED_CLOSED = 60; // spots kept for windows that aren't open
    constexpr int    SAVE_DELAY   = 1000; // ms after the last change

    using Renames = std::vector<std::pair<std::string, std::string>>; // old name, new name

    // State kept by workspace name, moved to the new names. All taken out,
    // then all put back: two workspaces can swap names.
    template <typename T>
    static void rename(std::unordered_map<std::string, T>& state, const Renames& renames) {
        std::vector<std::pair<std::string, T>> moving;
        for (const auto& [from, to] : renames)
            if (const auto IT = state.find(from); IT != state.end()) {
                moving.emplace_back(to, IT->second);
                state.erase(IT);
            }
        for (auto& [to, value] : moving)
            state[to] = value;
    }

    // A window still open (the plugin reloaded): its own entry.
    static auto exactEntry(std::vector<Store::SWindow>& pool, const PHLWINDOW& window) {
        return std::ranges::find_if(pool, [&](const Store::SWindow& e) {
            return e.address == reinterpret_cast<uint64_t>(window.get()) && e.pid == window->getPID() && e.app == window->m_initialClass;
        });
    }

    static CBox goalBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)};
    }

    static bool onCanvasWorkspace(const PHLWINDOW& window) {
        return window && window->m_isMapped && !window->m_pinned && window->m_workspace && !window->m_workspace->m_isSpecialWorkspace;
    }

    Vector2D CCanvas::scrolledFor(const PHLWORKSPACE& workspace) const {
        if (!workspace)
            return {};
        const auto IT = m_scrolled.find(workspace->m_name);
        return IT == m_scrolled.end() ? Vector2D{} : IT->second;
    }

    void CCanvas::noteOriginal(PHLWINDOW window) {
        // Full screen, its box is the screen's: it's noted once it's back on the canvas.
        if (!window || Fullscreen::controller()->isFullscreen(window) || std::ranges::any_of(m_originals, [&](const STracked& o) { return o.window.lock() == window; }))
            return;
        const auto TARGET = window->layoutTarget();
        if (!TARGET)
            return;
        const CBox BOX = goalBox(window);
        m_originals.push_back({
            .window    = window,
            .wasTiled  = !TARGET->floating(),
            .box       = TARGET->position(),
            .preferred = BOX.size(),
            .home      = BOX.pos() + scrolledFor(window->m_workspace),
            .workspace = window->m_workspace,
        });
    }

    void CCanvas::followWorkspaces() {
        // Workspace names key the scroll and screen kept per workspace (and
        // the saved file). Hyprland renames a workspace now and then (one
        // whose monitor goes away can come back as another number): its state
        // goes with it.
        std::erase_if(m_workspaceNames, [](const auto& seen) { return !seen.first.lock(); });
        Renames renames;
        for (const auto& ref : State::workspaceState()->workspaces()) {
            const auto WORKSPACE = ref.lock();
            if (!WORKSPACE || WORKSPACE->inert())
                continue;
            const auto IT = std::ranges::find_if(m_workspaceNames, [&](const auto& seen) { return seen.first.lock() == WORKSPACE; });
            if (IT == m_workspaceNames.end())
                m_workspaceNames.emplace_back(WORKSPACE, WORKSPACE->m_name);
            else if (IT->second != WORKSPACE->m_name) {
                renames.emplace_back(IT->second, WORKSPACE->m_name);
                IT->second = WORKSPACE->m_name;
            }
        }
        rename(m_scrolled, renames);
        rename(m_lastScreens, renames);

        // A window moved to another workspace (a bind, say) has its spot on
        // that workspace's canvas where it is now.
        for (auto& entry : m_originals) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW || !WINDOW->m_workspace || entry.workspace.lock() == WINDOW->m_workspace)
                continue;
            entry.workspace = WINDOW->m_workspace;
            if (!Fullscreen::controller()->isFullscreen(WINDOW))
                entry.home = goalBox(WINDOW).pos() + scrolledFor(WINDOW->m_workspace);
        }
    }

    void CCanvas::scheduleSave() {
        if (m_paused || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        if (!m_saveTimer)
            m_saveTimer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void* data) {
                    sc<CCanvas*>(data)->saveNow();
                    return 0;
                },
                this);
        wl_event_source_timer_update(m_saveTimer, SAVE_DELAY);
    }

    void CCanvas::stopSaving() {
        if (m_saveTimer)
            wl_event_source_remove(m_saveTimer);
        m_saveTimer = nullptr;
        if (m_relayoutTimer)
            wl_event_source_remove(m_relayoutTimer);
        m_relayoutTimer = nullptr;
        if (m_animationTimer)
            wl_event_source_remove(m_animationTimer);
        m_animationTimer = nullptr;
        if (m_fullscreenTimer)
            wl_event_source_remove(m_fullscreenTimer);
        m_fullscreenTimer = nullptr;
    }

    void CCanvas::saveNow() {
        if (m_paused)
            return;
        followWorkspaces();

        Store::SLayout layout;
        layout.autoCentre = m_autoCentre;
        layout.hotCorner  = g_hotCorner.choice;
        layout.wallpaper  = m_wallpaper;
        for (const auto& [name, offset] : m_scrolled)
            layout.scrolled.emplace_back(name, offset);

        std::erase_if(m_originals, [](const STracked& o) { return !onCanvasWorkspace(o.window.lock()); });
        noteSizes();
        updateHome();
        for (const auto& original : m_originals) {
            const auto WINDOW = original.window.lock();
            // Full screen, it's saved at its spot on the canvas.
            const CBox FRAME = Fullscreen::controller()->isFullscreen(WINDOW) ? CBox{original.home, original.preferred} :
                                                                                goalBox(WINDOW).translate(scrolledFor(WINDOW->m_workspace));
            layout.windows.push_back({
                .app       = WINDOW->m_initialClass,
                .title     = Store::titleKey(WINDOW->m_initialTitle),
                .workspace = WINDOW->m_workspace->m_name,
                .frame     = FRAME,
                .wasTiled  = original.wasTiled,
                .original  = original.box,
                .preferred = original.preferred,
                .home      = original.home,
                .address   = reinterpret_cast<uint64_t>(WINDOW.get()),
                .pid       = WINDOW->getPID(),
            });
        }
        // Windows that aren't open keep their saved spots.
        for (size_t i = 0; i < std::min(m_pool.size(), SAVED_CLOSED); ++i)
            layout.windows.push_back(m_pool[i]);

        Store::save(layout);
    }

    bool CCanvas::placeFromSaved(PHLWINDOW window, bool exactOnly) {
        std::set<uint64_t> open;
        for (const auto& w : Desktop::windowState()->windows())
            if (w != window)
                open.insert(reinterpret_cast<uint64_t>(w.get()));

        const uint64_t ADDRESS = reinterpret_cast<uint64_t>(window.get());
        const int64_t  PID     = window->getPID();
        std::optional<Store::SWindow> match;
        if (exactOnly) {
            const auto IT = exactEntry(m_pool, window);
            if (IT != m_pool.end()) {
                match = *IT;
                m_pool.erase(IT);
            }
        } else
            match = Store::takeMatch(window->m_initialClass, window->m_initialTitle, ADDRESS, PID, open, m_pool);
        if (!match)
            return false;

        const auto TARGET = window->layoutTarget();
        if (!TARGET || Fullscreen::controller()->isFullscreen(window))
            return false;

        // Where it was before the canvas: the saved record, or what it is now.
        if (!tracked(window)) {
            if (match->original)
                m_originals.push_back({.window = window, .wasTiled = match->wasTiled, .box = *match->original, .workspace = window->m_workspace});
            else
                noteOriginal(window);
        }
        if (auto* entry = tracked(window)) {
            entry->preferred = match->preferred.value_or(match->frame.size());
            entry->home      = match->home.value_or(match->frame.pos());
            entry->lastSized = match->frame.size();
        }

        if (!TARGET->floating())
            g_layoutManager->changeFloatingMode(TARGET);

        // The saved size too, and the spot relative to this workspace's scroll.
        const CBox FRAME  = match->frame.copy().translate(-scrolledFor(window->m_workspace));
        CBox       layout = TARGET->position();
        const CBox NOW    = goalBox(window);
        layout.x          = std::round(FRAME.x + (layout.x - NOW.x));
        layout.y          = std::round(FRAME.y + (layout.y - NOW.y));
        layout.w += FRAME.w - NOW.w;
        layout.h += FRAME.h - NOW.h;
        g_layoutManager->setTargetGeom(layout, TARGET);
        TARGET->warpPositionSize();
        damage();
        return true;
    }

    void CCanvas::restoreCanvas() {
        // Full screen already: back from it gets the view (onFullscreen).
        for (const auto& window : Desktop::windowState()->windows())
            if (window->m_isMapped && Fullscreen::controller()->isFullscreen(window))
                m_wentFullscreen.emplace_back(window);

        // Each monitor's workspace is laid out for that monitor as it is.
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor || !monitor->m_enabled || !monitor->m_activeWorkspace)
                continue;
            SOnMonitor here{*this, monitor};
            m_lastScreens[monitor->m_activeWorkspace->m_name] = usableArea();
        }
        const auto LAYOUT = Store::load();
        if (!LAYOUT)
            return;

        m_autoCentre       = LAYOUT->autoCentre;
        m_wallpaper        = LAYOUT->wallpaper;
        g_hotCorner.choice = LAYOUT->hotCorner;
        for (const auto& [name, offset] : LAYOUT->scrolled)
            m_scrolled[name] = offset;
        m_pool = LAYOUT->windows;

        // A workspace can come back under another name: unloading reloads
        // Hyprland's config, which can number a monitor's workspaces afresh.
        // Its open windows say what it's called now; its scroll goes with it.
        Renames renames;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!onCanvasWorkspace(window))
                continue;
            const auto        IT  = exactEntry(m_pool, window);
            const std::string NOW = window->m_workspace->m_name;
            if (IT != m_pool.end() && IT->workspace != NOW && !std::ranges::contains(renames, std::pair{IT->workspace, NOW}))
                renames.emplace_back(IT->workspace, NOW);
        }
        rename(m_scrolled, renames);

        // Windows still open (the plugin reloaded) find their exact entry
        // first, so two windows of one app don't swap spots; then the rest by
        // app and title.
        std::vector<PHLWINDOW> rest;
        for (const auto& window : Desktop::windowState()->windows())
            if (onCanvasWorkspace(window) && !placeFromSaved(window, true))
                rest.push_back(window);
        for (const auto& window : rest)
            placeFromSaved(window, false);
    }

    void CCanvas::onWindowClose(PHLWINDOW window) {
        const auto IT = std::ranges::find_if(m_originals, [&](const STracked& o) { return o.window.lock() == window; });
        if (IT == m_originals.end() || !window->m_workspace)
            return;

        // Its spot is kept, first in line, for when the app opens it again.
        m_pool.insert(m_pool.begin(), {
                                          .app       = window->m_initialClass,
                                          .title     = Store::titleKey(window->m_initialTitle),
                                          .workspace = window->m_workspace->m_name,
                                          .frame     = Fullscreen::controller()->isFullscreen(window) ? CBox{IT->home, IT->preferred} :
                                                                                                           goalBox(window).translate(scrolledFor(window->m_workspace)),
                                          .wasTiled  = IT->wasTiled,
                                          .original  = IT->box,
                                          .preferred = IT->preferred,
                                          .home      = IT->home,
                                      });
        m_originals.erase(IT);
        scheduleSave();
    }

    void CCanvas::putWindowsBack() {
        // Every window the canvas moved goes back where it was before it:
        // tiled ones back into the layout, floating ones to their old spot.
        for (const auto& original : m_originals) {
            const auto WINDOW = original.window.lock();
            const auto TARGET = WINDOW && WINDOW->m_isMapped ? WINDOW->layoutTarget() : nullptr;
            if (!TARGET || Fullscreen::controller()->isFullscreen(WINDOW))
                continue;
            if (original.wasTiled) {
                if (TARGET->floating())
                    g_layoutManager->changeFloatingMode(TARGET);
            } else if (TARGET->floating()) {
                g_layoutManager->setTargetGeom(original.box, TARGET);
                TARGET->warpPositionSize();
            }
        }
    }

    void CCanvas::pause() {
        if (m_paused)
            return;
        stopSaving();
        saveNow();
        m_paused = true;
        reset();
        m_unplaced.clear();
        m_wentFullscreen.clear(); // noted again on resume
        putWindowsBack();
        m_originals.clear();
        m_pool.clear();
        m_scrolled.clear();
    }

    void CCanvas::resume() {
        if (!m_paused)
            return;
        m_paused = false;
        restoreCanvas();
    }

    bool CCanvas::paused() const {
        return m_paused;
    }

    void CCanvas::shutdown() {
        stopSaving();
        if (!m_paused) {
            saveNow();
            reset();
            putWindowsBack();
        }
        m_originals.clear();
        m_pool.clear();
        m_unplaced.clear();
    }

    void CCanvas::restoreOffscreen() {
        // Every window that isn't fully in view comes back into it, fanned out.
        if (m_paused || m_state == STATE_GLIDING)
            return;
        const CBox AREA = live() ? viewInCanvas() : usableArea();
        int        i    = 0;
        for (const auto& window : canvasWindows()) {
            const CBox BOX = goalBox(window);
            if (BOX.x >= AREA.x && BOX.y >= AREA.y && BOX.x + BOX.w <= AREA.x + AREA.w && BOX.y + BOX.h <= AREA.y + AREA.h)
                continue;
            moveWindow(window, {AREA.x + AREA.w / 2 - BOX.w / 2 + (i % 8) * 28, AREA.y + 40 + (i % 8) * 28});
            ++i;
        }
        scheduleSave();
    }
}
