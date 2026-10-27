#pragma once

#include <hyprland/src/SharedDefs.hpp>

namespace Expanse::Render {
    // Draws the canvas from Hyprland's render stages.
    void onStage(eRenderStage stage);
    // Windows parked off their own monitor, over another one, take no pointer
    // input there; with `active` false, none are.
    void updateParking(bool active);
    // Puts every window back as Hyprland had it and drops cached textures.
    void shutdown();
}
