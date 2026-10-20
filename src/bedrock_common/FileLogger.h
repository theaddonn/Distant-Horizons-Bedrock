#pragma once

#include <cstdarg>

namespace dh::bedrock {

enum class LogLevel { Debug, Info, Warn, Error };

/// Opens `<modDir>/logs/dh.log`. A new session starts a fresh file (the previous one becomes dh.log.1, then
/// dh.log.2); a file also rotates at 8 MB. Safe to call repeatedly.
void initLog();

/// printf-style logging: thread-safe, flushed on every call, also forwarded to the LeviLamina logger.
void log(LogLevel level, char const* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

void logv(LogLevel level, char const* fmt, va_list args);

} // namespace dh::bedrock

#define DH_LOG_D(...) ::dh::bedrock::log(::dh::bedrock::LogLevel::Debug, __VA_ARGS__)
#define DH_LOG_I(...) ::dh::bedrock::log(::dh::bedrock::LogLevel::Info, __VA_ARGS__)
#define DH_LOG_W(...) ::dh::bedrock::log(::dh::bedrock::LogLevel::Warn, __VA_ARGS__)
#define DH_LOG_E(...) ::dh::bedrock::log(::dh::bedrock::LogLevel::Error, __VA_ARGS__)
