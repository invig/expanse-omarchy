#define WLR_USE_UNSTABLE

#include "Canvas.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <ranges>
#include <set>

// Remembering the canvas, and switching in and out cleanly, as on the Mac.
namespace Expanse {

    using Desktop::View::IGeometric;

    constexpr size_t SAVED_CLOSED = 60; // spots kept for windows that aren't open
    constexpr int    SAVE_DELAY   = 1000; // ms after the last change

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
        if (!window || std::ranges::any_of(m_originals, [&](const STracked& o) { return o.window.lock() == window; }))
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
        });
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
    }

    void CCanvas::saveNow() {
        if (m_paused)
            return;

        Store::SLayout layout;
        layout.autoCentre = m_autoCentre;
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
            const auto IT = std::ranges::find_if(m_pool, [&](const Store::SWindow& e) { return e.address == ADDRESS && e.pid == PID && e.app == window->m_initialClass; });
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
                m_originals.push_back({.window = window, .wasTiled = match->wasTiled, .box = *match->original});
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
        m_lastScreen = usableArea();
        const auto LAYOUT = Store::load();
        if (!LAYOUT)
            return;

        m_autoCentre = LAYOUT->autoCentre;
        for (const auto& [name, offset] : LAYOUT->scrolled)
            m_scrolled[name] = offset;
        m_pool = LAYOUT->windows;

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
                                          .frame     = goalBox(window).translate(scrolledFor(window->m_workspace)),
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
