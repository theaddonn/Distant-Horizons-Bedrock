#include "bedrock_common/ConfigBridge.h"

#include "dh/platform/ILogger.h"

#include <cstdlib>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // std::getenv
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
        // Java WorldGenPlanConfigEventHandler: a plan with surface (rough) generation needs an IRoughGenerator,
        // which Bedrock does not provide -> CHUNKS_ONLY. Otherwise every coarse retrieval fails with
        // "No rough generator provided" and nothing beyond the loaded chunks is ever generated.
        {
            auto& plan = store_.options().common.worldGenerator.generatorPlan;
            userPlan_  = plan; // what the file says: the forced values below are never written back
            if (surfaceGenEnabled(plan)) {
                log::warn(
                    "config: generatorPlan needs a rough surface generator (not available on Bedrock); "
                    "using ChunksOnly"
                );
                plan = GeneratorPlan::ChunksOnly;
            }
            if (!store_.options().common.worldGenerator.allowUnvalidatedGenerator && plan != GeneratorPlan::Disabled) {
                log::warn(
                    "config: distant generation forced to Disabled until scratch chunk generation is validated "
                    "in game; set common.worldGenerator.allowUnvalidatedGenerator=true in config.json to enable "
                    "generatorPlan again"
                );
                plan = GeneratorPlan::Disabled;
            }
            planForced_ = plan != userPlan_;
            forcedPlan_ = plan;
        }
        // compareMode: the generator reads the env switch until it takes the option directly. Set before anything
        // can start a generator; an explicit env value wins.
        if (store_.options().common.worldGenerator.compareMode && std::getenv("DH_BEDROCK_GEN_COMPARE") == nullptr) {
            _putenv_s("DH_BEDROCK_GEN_COMPARE", "1");
            log::info("config: common.worldGenerator.compareMode=true -> DH_BEDROCK_GEN_COMPARE=1");
        }
        store_.onChange([this](const Options&) { (void)save(); });
    }
    if (writeDefaults) (void)save();
}

Status ConfigBridge::save() {
    std::lock_guard lock(mutex_);
    if (path_.empty()) return Status(ErrorCode::InvalidArgument, "ConfigBridge: init() not called");
    // Keep the user's plan in the file: the safety overrides in init() only apply to the running process, so
    // flipping allowUnvalidatedGenerator later does not also require retyping generatorPlan.
    std::string text;
    if (planForced_ && store_.options().common.worldGenerator.generatorPlan == forcedPlan_) {
        // Serialise a copy: the live options are read by other threads and must never show the unforced plan.
        dh::ConfigStore copy(store_.cores());
        dh::Options     o = store_.options();
        o.common.worldGenerator.generatorPlan = userPlan_;
        copy.set(o);
        text = copy.save();
    } else {
        planForced_ = false; // changed at runtime: persist as is
        text        = store_.save();
    }
    auto              st   = fs_.writeFileAtomic(
        path_,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size())
    );
    if (!st) log::warn("config: save failed: " + st.error().message);
    return st;
}

} // namespace dh::bedrock
