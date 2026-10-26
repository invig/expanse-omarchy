#pragma once

#include <hyprland/src/SharedDefs.hpp>

namespace Expanse::Render {
    // Draws the canvas from Hyprland's render stages.
    void onStage(eRenderStage stage);
    // Puts every window back as Hyprland had it and drops cached textures.
    void shutdown();
}
