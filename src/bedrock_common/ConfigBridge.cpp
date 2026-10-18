#include "bedrock_common/ConfigBridge.h"

#include "dh/platform/ILogger.h"

#include <span>
#include <string>

namespace dh::bedrock {

ConfigBridge& ConfigBridge::instance() {
    static ConfigBridge bridge;
    return bridge;
}

void ConfigBridge::init(const std::filesystem::path& configDir) {
    bool writeDefaults = false;
    {
        std::lock_guard lock(mutex_);
        if (initialised_) return;
        initialised_ = true;
        path_        = configDir / "config.json";
        (void)fs_.mkdirs(configDir);
        auto bytes = fs_.readFile(path_);
        if (!bytes) {
            if (bytes.error().code != ErrorCode::NotFound)
                log::warn("config: cannot read config.json (" + bytes.error().message + "); using defaults");
            writeDefaults = true;
        } else {
            const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            if (auto err = store_.load(text)) {
                log::warn("config: config.json is not valid (" + *err + "); using defaults");
                writeDefaults = true;
            }
            for (const auto& w : store_.warnings()) log::warn("config: " + w);
        }
        store_.onChange([this](const Options&) { (void)save(); });
    }
    if (writeDefaults) (void)save();
}

Status ConfigBridge::save() {
    std::lock_guard lock(mutex_);
    if (path_.empty()) return Status(ErrorCode::InvalidArgument, "ConfigBridge: init() not called");
    const std::string text = store_.save();
    auto              st   = fs_.writeFileAtomic(
        path_,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size())
    );
    if (!st) log::warn("config: save failed: " + st.error().message);
    return st;
}

} // namespace dh::bedrock
