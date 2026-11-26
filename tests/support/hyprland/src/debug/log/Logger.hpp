#pragma once

#include <format>
#include <string>
#include <string_view>

// Hyprland's logger is not part of this repository. Log.cpp reports each line
// to it after the line has already been appended to the state-directory log.
// This stand-in only satisfies that call so the test can run Log.cpp's own
// file creation. It does not create files and it does not choose their mode.
namespace Log {

    enum eLogLevel { DEBUG, INFO, WARN, ERR, CRIT, TRACE };

    class CLogger {
      public:
        void log(eLogLevel, std::string_view) {}

        template <typename... Args>
        void log(eLogLevel, std::format_string<Args...> fmt, Args&&... args) {
            (void)std::format(fmt, std::forward<Args>(args)...);
        }
    };

    inline CLogger g_testLogger;
    inline CLogger* logger = &g_testLogger;
}
