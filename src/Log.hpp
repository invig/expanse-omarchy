#pragma once

#include <format>
#include <string>

// Expanse's log, as on the Mac. Hyprland keeps no log of its own by default
// (debug:disable_logs), so the plugin keeps this run's recent lines itself
// (`hyprctl expanse log`), and with Detailed Logging on writes every line to
// ~/.local/state/expanse/expanse.log too, capped, the older half kept as
// expanse.1.log. Window titles stay out of it: windows are named by app and
// address.
namespace Expanse::Log {

    void line(const std::string& text);

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        line(std::format(fmt, std::forward<Args>(args)...));
    }

    // This run's lines, newest last, at most `maxChars` of the newest.
    std::string recent(size_t maxChars);

    void setFile(bool on);
    bool fileOn();
    std::string filePath();
}
