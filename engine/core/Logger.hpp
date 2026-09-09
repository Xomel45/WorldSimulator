#pragma once

#include <format>
#include <print>
#include <string>

namespace wsim {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    static void setLevel(LogLevel level) noexcept { s_minLevel = level; }

    // WSIM_STRIP_LOGS (release) — debug/info компилируются в no-op: их
    // форматные строки становятся неиспользуемыми и вычищаются из бинаря
    // (dead-string elimination + LTO). warn/error остаются для диагностики.
    template<typename... Args>
    static void debug([[maybe_unused]] std::format_string<Args...> fmt,
                      [[maybe_unused]] Args&&... args) {
#ifndef WSIM_STRIP_LOGS
        log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
#endif
    }

    template<typename... Args>
    static void info([[maybe_unused]] std::format_string<Args...> fmt,
                     [[maybe_unused]] Args&&... args) {
#ifndef WSIM_STRIP_LOGS
        log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
#endif
    }

    template<typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    inline static LogLevel s_minLevel = LogLevel::Debug;

    static void log(LogLevel level, std::string msg) {
        if (level < s_minLevel) return;

        switch (level) {
        case LogLevel::Debug:
            std::println("\033[37m[DBG] {}\033[0m", msg);
            break;
        case LogLevel::Info:
            std::println("\033[32m[INF] {}\033[0m", msg);
            break;
        case LogLevel::Warn:
            std::println("\033[33m[WRN] {}\033[0m", msg);
            break;
        case LogLevel::Error:
            std::println(stderr, "\033[31m[ERR] {}\033[0m", msg);
            break;
        }
    }
};

}  // namespace wsim
