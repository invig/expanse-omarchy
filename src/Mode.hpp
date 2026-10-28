#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

// Expanse as a Hyprland layout, "expanse": a workspace with that layout (from
// general.layout, a workspace rule, or Omarchy's workspace mode of that name)
// is a canvas, and every other workspace is left to its own layout. Windows
// arriving tiled on it float where they are; when it takes another layout,
// they tile again.
namespace Expanse::Mode {
    bool add(HANDLE handle);
    void remove(HANDLE handle);
    bool isExpanse(const PHLWORKSPACE& workspace);
}
