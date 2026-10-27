#pragma once

#include <hyprland/src/SharedDefs.hpp>

namespace Expanse::Render {
    // Draws the canvas from Hyprland's render stages.
    void onStage(eRenderStage stage);
    // A window reaching from its own monitor onto the one with the pointer
    // takes no pointer input there; with `active` false, none are blocked.
    void updateParking(bool active, const Vector2D& pointer);
    // Puts every window back as Hyprland had it and drops cached textures.
    void shutdown();
}
