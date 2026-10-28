#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

namespace Expanse::Render {
    // Draws the canvas from Hyprland's render stages.
    void onStage(eRenderStage stage);
    // Before a monitor's frame: what changed on the desktop's minimap is redrawn.
    void beforeFrame(const PHLMONITOR& monitor);
    // A window reaching from its own monitor onto the one with the pointer
    // takes no pointer input there; with `active` false, none are blocked.
    void updateParking(bool active, const Vector2D& pointer);
    // Soon, while nothing moves: windows' names made ready for the canvas.
    void prepareLabels();
    // Puts every window back as Hyprland had it and drops cached textures.
    void shutdown();
}
