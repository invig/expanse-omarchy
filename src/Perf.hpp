#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <chrono>
#include <string>

// Frame timing, to find jank: `hyprctl expanse perf start`, do something,
// `hyprctl expanse perf stop` prints every monitor's frames as JSON. With
// `start gpu`, each frame also waits for the GPU to finish it, to time that.
namespace Expanse::Perf {
    void        start(bool gpu);
    std::string stop();
    bool        recording();

    void        frameStart(const PHLMONITOR& monitor); // render.pre
    void        frameEnd();                            // the pass has drawn
    void        textRendered();

    // The plugin's own share of the frame being drawn: the time this lives.
    class CPluginTime {
      public:
        CPluginTime();
        ~CPluginTime();

      private:
        std::chrono::steady_clock::time_point m_start;
    };
}
