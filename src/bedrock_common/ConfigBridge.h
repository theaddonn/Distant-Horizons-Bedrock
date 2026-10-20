#pragma once

#include "dh/config/ConfigStore.h"
#include "dh/platform/IFileSystem.h"

#include <filesystem>
#include <mutex>

namespace dh::bedrock {

/// Owns the process ConfigStore and persists it to `<configDir>/config.json`.
class ConfigBridge {
public:
    static ConfigBridge& instance();

    /// Loads `<configDir>/config.json`. Missing or corrupt file -> defaults + warning
    /// (the defaults are written out). Afterwards every change is saved. Idempotent.
    void init(const std::filesystem::path& configDir);

    dh::ConfigStore& config() { return store_; }

    /// Writes the current options now (also done automatically on change).
    Status save();

private:
    ConfigBridge() = default;
    std::mutex            mutex_;
    dh::ConfigStore       store_;
    dh::StdFileSystem     fs_;
    std::filesystem::path path_;
    bool                  initialised_ = false;
    dh::GeneratorPlan     userPlan_    = dh::GeneratorPlan::SurfaceThenChunks; ///< plan as loaded from the file
    dh::GeneratorPlan     forcedPlan_  = dh::GeneratorPlan::SurfaceThenChunks; ///< plan after the safety overrides
    bool                  planForced_  = false;
};

} // namespace dh::bedrock
