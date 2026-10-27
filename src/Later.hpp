#pragma once

#include <functional>

namespace Expanse {
    // Hyprland's doLater, for work that has to wait for the event that asked
    // for it to finish. Whatever hasn't run when the plugin unloads is
    // cancelled: its code goes with the plugin.
    void later(std::function<void()> fn);
    void cancelLater();
}
