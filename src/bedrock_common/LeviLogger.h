#pragma once

#include "dh/platform/ILogger.h"

namespace dh::bedrock {

/// dh::ILogger forwarding to FileLogger (dh.log), which also forwards to the LeviLamina logger.
class LeviLogger final : public ILogger {
public:
    void log(dh::LogLevel level, std::string_view message) override;
    bool enabled(dh::LogLevel level) const override { return level >= dh::LogLevel::Debug; }

    /// Process-wide instance; install with `dh::log::set(&LeviLogger::instance())`.
    static LeviLogger& instance();
};

/// initLog() + dh::log::set(LeviLogger). Safe to call repeatedly.
void installCoreLogger();

} // namespace dh::bedrock
