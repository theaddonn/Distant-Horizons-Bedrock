#pragma once

// Logging seam between dh_core and the platform. The core never touches a log
// file or the LeviLamina logger directly; it calls dh::log::* and the glue
// installs an ILogger (FileLogger on Bedrock, StderrLogger in tests).
//
// Thread safety: ILogger::log may be called from any thread concurrently.
// dh::log::set() is expected at startup/shutdown only (the pointer is atomic,
// but the logger's lifetime is the caller's problem).

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace dh {

enum class LogLevel : std::uint8_t { Trace = 0, Debug, Info, Warn, Error };

inline const char* logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "?";
}

struct ILogger {
    virtual ~ILogger() = default;
    /// `message` is a single line without trailing newline.
    virtual void log(LogLevel level, std::string_view message) = 0;
    /// Cheap pre-check so callers can skip expensive formatting.
    virtual bool enabled(LogLevel) const { return true; }
};

/// Drops everything. The default until the platform installs a logger.
struct NullLogger final : ILogger {
    void log(LogLevel, std::string_view) override {}
    bool enabled(LogLevel) const override { return false; }
};

/// Writes "[LEVEL] message\n" to stderr. Handy for host tests and tools.
struct StderrLogger final : ILogger {
    LogLevel minLevel = LogLevel::Trace;
    void     log(LogLevel level, std::string_view message) override {
        if (level < minLevel) return;
        std::fprintf(stderr, "[%s] %.*s\n", logLevelName(level), static_cast<int>(message.size()), message.data());
    }
    bool enabled(LogLevel level) const override { return level >= minLevel; }
};

namespace log {

namespace detail {
inline NullLogger& nullLogger() {
    static NullLogger instance;
    return instance;
}
inline std::atomic<ILogger*>& slot() {
    static std::atomic<ILogger*> logger{&nullLogger()};
    return logger;
}
} // namespace detail

/// Installs `logger` (nullptr restores the NullLogger). Returns the previous logger.
inline ILogger* set(ILogger* logger) {
    return detail::slot().exchange(logger ? logger : &detail::nullLogger(), std::memory_order_acq_rel);
}
inline ILogger& get() { return *detail::slot().load(std::memory_order_acquire); }
inline bool     enabled(LogLevel level) { return get().enabled(level); }

inline void write(LogLevel level, std::string_view message) { get().log(level, message); }
inline void trace(std::string_view message) { write(LogLevel::Trace, message); }
inline void debug(std::string_view message) { write(LogLevel::Debug, message); }
inline void info(std::string_view message) { write(LogLevel::Info, message); }
inline void warn(std::string_view message) { write(LogLevel::Warn, message); }
inline void error(std::string_view message) { write(LogLevel::Error, message); }

#if defined(__clang__) || defined(__GNUC__)
#define DH_PRINTF_LIKE(fmtIndex, firstArg) __attribute__((format(printf, fmtIndex, firstArg)))
#else
#define DH_PRINTF_LIKE(fmtIndex, firstArg)
#endif

/// printf-style convenience. Messages longer than 1023 bytes are truncated.
inline void logf(LogLevel level, const char* fmt, ...) DH_PRINTF_LIKE(2, 3);
inline void logf(LogLevel level, const char* fmt, ...) {
    if (!enabled(level)) return;
    char         buffer[1024];
    std::va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n < 0) return;
    std::size_t len = static_cast<std::size_t>(n);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    write(level, std::string_view(buffer, len));
}

#undef DH_PRINTF_LIKE

} // namespace log
} // namespace dh
