#define WLR_USE_UNSTABLE

#include "Canvas.hpp"
#include "Later.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

// The workspaces bar, Expanse's Spaces bar: every workspace on the canvas's
// monitor beside the minimap. Each workspace has a canvas of its own; going to
// one with the canvas up (its tile, or a workspace bind) brings the canvas
// along, zoomed out, so you can pick a window there.
namespace Expanse {

    using Desktop::View::IGeometric;

    constexpr Vector2D TILE_SIZE = {104.0, 65.0}; // the screen's shape, roughly
    constexpr double   TILE_GAP  = 10.0;
    constexpr double   TILE_TOP  = 37.0;          // below the heading
    constexpr double   BAR_GAP   = 12.0;          // from the minimap
    constexpr double   EDGE      = 16.0;          // from the screen's edge

    static CBox goalBox(const PHLWINDOW& window) {
        return {window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)};
    }

    // Omarchy gives each monitor workspaces 1-10 of its own (41-50 on the
    // fifth, say), Super + 1-0 on that monitor, and its bar shows them as
    // 1-10. Its file of which monitor has which block says it does.
    static bool perMonitorWorkspaces() {
        static const bool OMARCHY = [] {
            const char*       STATE = std::getenv("XDG_STATE_HOME");
            const char*       HOME  = std::getenv("HOME");
            const std::string DIR   = STATE && *STATE ? std::string{STATE} : std::string{HOME ? HOME : ""} + "/.local/state";
            std::error_code   error;
            return std::filesystem::exists(DIR + "/omarchy/monitor-workspaces", error);
        }();
        return OMARCHY;
    }

    // "3" → "Workspace 3" (47 → "Workspace 7" with Omarchy's numbering); named
    // workspaces keep their name.
    static std::string spaceName(const PHLWORKSPACE& workspace) {
        const auto& NAME = workspace->m_name;
        if (NAME.empty() || !std::ranges::all_of(NAME, [](unsigned char c) { return std::isdigit(c); }))
            return NAME;
        if (perMonitorWorkspaces() && workspace->m_id > 0)
            return "Workspace " + std::to_string((workspace->m_id - 1) % 10 + 1);
        return "Workspace " + NAME;
    }

    std::optional<size_t> SSpacesBar::tileAt(const Vector2D& pos) const {
        // The tile or its name.
        for (size_t i = 0; i < tiles.size(); ++i)
            if (CBox{tiles[i].box.x - 4, tiles[i].box.y - 18, tiles[i].box.w + 8, tiles[i].box.h + 36}.containsPoint(pos))
                return i;
        return std::nullopt;
    }

    std::optional<SSpacesBar> CCanvas::spacesBar() const {
        const auto MAP = minimap();
        return MAP ? spacesBarBeside(monitor(), workspace(), *MAP) : std::nullopt;
    }

    std::optional<SSpacesBar> CCanvas::spacesBarBeside(const PHLMONITOR& on, const PHLWORKSPACE& current, const SMinimap& map) const {
        const auto& MONITOR = on;
        const auto& CURRENT = current;
        const auto* MAP     = &map;
        if (!MONITOR)
            return std::nullopt;

        std::vector<PHLWORKSPACE> workspaces;
        for (const auto& workspace : State::workspaceState()->workspaces())
            if (workspace && !workspace->inert() && !workspace->m_isSpecialWorkspace && workspace->m_monitor == MONITOR)
                workspaces.push_back(workspace.lock());
        if (workspaces.size() < 2)
            return std::nullopt;
        // Numbered workspaces in order, then named ones.
        std::ranges::sort(workspaces, [](const PHLWORKSPACE& a, const PHLWORKSPACE& b) {
            if ((a->m_id > 0) != (b->m_id > 0))
                return a->m_id > 0;
            return a->m_id > 0 ? a->m_id < b->m_id : a->m_name < b->m_name;
        });

        // To the minimap's left, smaller if they wouldn't fit.
        const double   COUNT  = workspaces.size();
        const double   ROOM   = MAP->box.x - BAR_GAP - (MONITOR->m_position.x + EDGE);
        const double   SHRINK = std::clamp((ROOM - TILE_GAP - COUNT * TILE_GAP) / (COUNT * TILE_SIZE.x), 0.3, 1.0);
        const Vector2D TILE   = TILE_SIZE * SHRINK;

        SSpacesBar bar;
        bar.box.w = TILE_GAP + workspaces.size() * (TILE.x + TILE_GAP);
        bar.box.h = MAP->box.h;
        bar.box.x = MAP->box.x - BAR_GAP - bar.box.w;
        bar.box.y = MAP->box.y;

        const auto HOVER   = m_hoverSpace.lock();
        const CBox SCREEN  = {MONITOR->m_position, MONITOR->m_size};
        for (size_t i = 0; i < workspaces.size(); ++i) {
            const auto& workspace = workspaces[i];
            auto&       tile      = bar.tiles.emplace_back();
            tile.workspace        = workspace;
            tile.name             = spaceName(workspace);
            tile.current          = workspace == CURRENT;
            tile.hovered          = workspace == HOVER;
            tile.box              = {bar.box.x + TILE_GAP + i * (TILE.x + TILE_GAP), bar.box.y + TILE_TOP + (TILE_SIZE.y - TILE.y), TILE.x, TILE.y};

            // A small map of the workspace: the screen and every window, at
            // their real (100%) spots.
            std::vector<CBox> windows;
            CBox              bounds = SCREEN;
            for (const auto& window : Desktop::windowState()->windows()) {
                if (!window->m_isMapped || window->isHidden() || window->m_pinned || window->m_workspace != workspace)
                    continue;
                windows.push_back(goalBox(window));
                const double X0 = std::min(bounds.x, windows.back().x), Y0 = std::min(bounds.y, windows.back().y);
                bounds = {X0, Y0, std::max(bounds.x + bounds.w, windows.back().x + windows.back().w) - X0, std::max(bounds.y + bounds.h, windows.back().y + windows.back().h) - Y0};
            }
            bounds.expand(std::max(bounds.w, bounds.h) * 0.03);

            const CBox     AREA  = tile.box.copy().expand(-4);
            const double   SCALE = std::min(AREA.w / std::max(bounds.w, 1.0), AREA.h / std::max(bounds.h, 1.0));
            const Vector2D AT    = {AREA.x + (AREA.w - bounds.w * SCALE) / 2, AREA.y + (AREA.h - bounds.h * SCALE) / 2};
            const auto     MAPPED = [&](const CBox& box) {
                return CBox{AT.x + (box.x - bounds.x) * SCALE, AT.y + (box.y - bounds.y) * SCALE, std::max(box.w * SCALE, 1.0), std::max(box.h * SCALE, 1.0)};
            };
            tile.screen = MAPPED(SCREEN);
            for (const auto& box : windows)
                tile.windows.push_back(MAPPED(box));
        }
        return bar;
    }

    bool CCanvas::switchTo(PHLWORKSPACE target) {
        // A workspace on another monitor has no tile here: the canvas stays on this one.
        if (m_paused || !target || target->m_isSpecialWorkspace || target->m_monitor != monitor())
            return false;
        if (!live())
            open();
        if (!live())
            return false;
        if (target == workspace())
            return true;
        // Out of the event that asked for it; the canvas follows once Hyprland
        // has switched (onWorkspaceChanged), as it does for a workspace bind.
        later([ref = PHLWORKSPACEREF{target}] {
            if (const auto WORKSPACE = ref.lock(); WORKSPACE && !WORKSPACE->inert())
                (void)Config::Actions::changeWorkspace(WORKSPACE); // failing, the canvas stays where it is
        });
        return true;
    }

    void CCanvas::onWorkspaceChanged(PHLWORKSPACE target) {
        if (m_paused || !target)
            return;
        if (!live()) {
            // A workspace last laid out on another screen (its monitor went
            // away, say) fits this one as it comes into view; one seen for
            // the first time notes the screen it's laid out for.
            later([this, ref = PHLWORKSPACEREF{target}] {
                const auto WORKSPACE = ref.lock();
                const auto MONITOR   = WORKSPACE ? WORKSPACE->m_monitor.lock() : nullptr;
                if (MONITOR && MONITOR->m_activeWorkspace == WORKSPACE && !live())
                    relayoutOn(MONITOR);
            });
            return;
        }
        if (target->m_monitor != m_monitor || target == m_workspace.lock())
            return;
        later([this, ref = PHLWORKSPACEREF{target}] {
            if (const auto WORKSPACE = ref.lock())
                followWorkspace(WORKSPACE);
        });
    }

    void CCanvas::followWorkspace(PHLWORKSPACE target) {
        const auto MONITOR = m_monitor.lock();
        const auto OLD     = m_workspace.lock();
        if (!live() || !MONITOR || MONITOR->m_activeWorkspace != target || OLD == target)
            return;

        // Zoomed out, the switch happens under the canvas: no slide.
        for (const auto& workspace : {OLD, target}) {
            if (!workspace)
                continue;
            workspace->m_renderOffset->warp();
            workspace->m_alpha->warp();
        }

        // The canvas you left settles where leaving it would have: where you
        // were, unless you'd moved around (or were gliding somewhere).
        const bool SHOWING = showsCanvas();
        settle(m_state == STATE_GLIDING ? m_to : m_returnCamera.value_or(centredOn(viewCentre().pos(), 1.0)));
        reset();
        scheduleSave();
        if (!SHOWING || !begin()) {
            damage();
            return;
        }

        // This workspace's canvas, zoomed out to every window.
        m_hover        = Desktop::focusState()->window();
        m_returnCamera = m_camera;
        m_state        = STATE_OVERVIEW;
        setCamera(fitCamera(OVERVIEW_MAX_SCALE));
        placeUnplaced();
    }
}
