#include "bedrock_common/LeviLogger.h"

#include "bedrock_common/FileLogger.h"

namespace dh::bedrock {

void LeviLogger::log(dh::LogLevel level, std::string_view message) {
    bedrock::LogLevel out = bedrock::LogLevel::Info;
    switch (level) {
    case dh::LogLevel::Trace:
    case dh::LogLevel::Debug:
        out = bedrock::LogLevel::Debug;
        break;
    case dh::LogLevel::Info:
        out = bedrock::LogLevel::Info;
        break;
    case dh::LogLevel::Warn:
        out = bedrock::LogLevel::Warn;
        break;
    case dh::LogLevel::Error:
        out = bedrock::LogLevel::Error;
        break;
    }
    bedrock::log(out, "%.*s", static_cast<int>(message.size()), message.data());
}

LeviLogger& LeviLogger::instance() {
    static LeviLogger logger;
    return logger;
}

void installCoreLogger() {
    initLog();
    dh::log::set(&LeviLogger::instance());
}

} // namespace dh::bedrock
