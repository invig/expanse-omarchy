#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <string>

// A short message near the bottom of the screen that fades out, for toggles,
// as on the Mac: a dark pill, 1.2 s, then a 0.3 s fade.
namespace Expanse::Toast {
    void show(const std::string& text); // on the focused monitor
    void draw(const PHLMONITOR& monitor); // on top of everything, while the frame is drawn
    void afterFrame(const PHLMONITOR& monitor); // while it fades, asks for the next frame
    void shutdown();
}
