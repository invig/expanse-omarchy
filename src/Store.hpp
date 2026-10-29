#pragma once

#include <hyprutils/math/Box.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Hyprutils::Math;

// The canvas, saved to $XDG_STATE_HOME/expanse/layout.json so it survives
// reloads and restarts, as Expanse keeps it on the Mac.
namespace Expanse::Store {

    struct SWindow {
        std::string         app;       // the window's initial class
        std::string         title;     // a hash of its initial title, see titleKey
        std::string         workspace; // its workspace's name
        CBox                frame;     // absolute canvas coordinates
        bool                wasTiled = false;
        std::optional<CBox> original; // where it was before the canvas, real coordinates
        // The size you chose, and the window's spot in the home layout (at
        // those sizes): windows shrink to fit a smaller screen and grow back.
        std::optional<Vector2D> preferred;
        std::optional<Vector2D> home;
        // While the app keeps running (Hyprland or the plugin reloaded), these
        // identify the window exactly, even among windows with the same title.
        uint64_t            address = 0;
        int64_t             pid     = 0;
    };

    struct SLayout {
        std::vector<std::pair<std::string, Vector2D>> scrolled; // per workspace
        std::vector<std::pair<std::string, Vector2D>> fitted;   // per workspace, the screen size it was fitted to
        std::vector<std::string>                      away;     // workspaces in another layout, their canvas kept (in `windows`)
        bool                                          autoCentre = true;
        // Choices from the menu; without one, the config value.
        std::optional<std::string>                    hotCorner;
        std::optional<bool>                           wallpaper;
        std::optional<bool>                           names;
        bool                                          logging = false;
        std::vector<SWindow>                          windows;
    };

    std::optional<SLayout> load();
    // False when the layout hasn't changed since it was last saved (nothing written).
    bool                   save(const SLayout& layout);

    // Earlier layouts to go back to (Restore Layout), newest first, next to
    // the layout in snapshots/, each named for when it was taken. One is taken
    // when the layout has changed and the newest is SNAPSHOT_SPACING old, so
    // they're minutes apart, not one per window move; only the newest
    // SNAPSHOTS_KEPT stay.
    struct SSnapshot {
        int64_t               taken = 0; // seconds since 1970
        std::filesystem::path file;
    };
    constexpr size_t       SNAPSHOTS_KEPT   = 3;
    constexpr int64_t      SNAPSHOT_SPACING = 15 * 60; // seconds
    std::vector<SSnapshot> snapshots();
    void                   saveSnapshot(const SLayout& layout);
    std::optional<SLayout> load(const SSnapshot& snapshot);

    // Window titles can be private (document names, mail subjects), so the
    // file stores a hash: enough to recognise the same window next time, not
    // to read the title.
    std::string titleKey(const std::string& title);

    // The saved entry for a window: the same window (same address in the same
    // process), then same app and title, then same app. A matched entry leaves
    // the pool so two windows never claim one spot, and `open` (addresses of
    // windows open now) keeps the fallbacks off entries another open window owns.
    std::optional<SWindow> takeMatch(const std::string& app, const std::string& title, uint64_t address, int64_t pid, const std::set<uint64_t>& open,
                                     std::vector<SWindow>& pool);
}
