#define WLR_USE_UNSTABLE

#include "Log.hpp"

#include <hyprland/src/debug/log/Logger.hpp>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>

namespace Expanse::Log {

    // Enough for a while of ordinary use, as the system log keeps a run's on the Mac.
    constexpr size_t   KEPT_LINES = 3000;
    constexpr uint64_t MAX_BYTES  = 5'000'000;

    static std::deque<std::string> g_lines;
    static bool                    g_file = false;

    static std::filesystem::path dir() {
        const char*           STATE = std::getenv("XDG_STATE_HOME");
        const char*           HOME  = std::getenv("HOME");
        std::filesystem::path base  = STATE && *STATE ? STATE : std::filesystem::path{HOME ? HOME : "/tmp"} / ".local/state";
        return base / "expanse";
    }

    std::string filePath() {
        return (dir() / "expanse.log").string();
    }

    static void toFile(const std::string& stamped) {
        std::error_code error;
        std::filesystem::create_directories(dir(), error);
        const auto PATH = std::filesystem::path{filePath()};
        // Over the cap, the older half is kept beside it, as on the Mac.
        if (std::filesystem::exists(PATH, error) && std::filesystem::file_size(PATH, error) > MAX_BYTES)
            std::filesystem::rename(PATH, dir() / "expanse.1.log", error);
        std::ofstream(PATH, std::ios::app) << stamped << '\n';
    }

    static std::string stamp() {
        const auto NOW = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        try {
            return std::format("{:%F %T}", std::chrono::current_zone()->to_local(NOW));
        } catch (...) { return std::format("{:%F %T} UTC", NOW); }
    }

    void line(const std::string& text) {
        const auto STAMPED = std::format("{} {}", stamp(), text);
        g_lines.push_back(STAMPED);
        while (g_lines.size() > KEPT_LINES)
            g_lines.pop_front();
        if (g_file)
            toFile(STAMPED);
        ::Log::logger->log(::Log::INFO, "[expanse] {}", text);
    }

    std::string recent(size_t maxChars) {
        std::string out;
        for (auto it = g_lines.rbegin(); it != g_lines.rend() && out.size() + it->size() + 1 <= maxChars; ++it)
            out.insert(0, *it + "\n");
        return out;
    }

    void setFile(bool on) {
        if (on == g_file)
            return;
        if (!on)
            line("detailed logging off");
        g_file = on;
        if (on)
            line("detailed logging on");
    }

    bool fileOn() {
        return g_file;
    }
}
