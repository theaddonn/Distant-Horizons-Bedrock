#include "dh/net/SessionConfig.h"

#include <algorithm>
#include <limits>

namespace dh::net {

namespace {

struct PlanFlags {
    bool generationEnabled, chunkGen, surfaceGen;
};

constexpr PlanFlags flagsOf(GeneratorPlan p) {
    switch (p) {
    case GeneratorPlan::SurfaceThenChunks:
        return {true, true, true};
    case GeneratorPlan::SurfaceOnly:
        return {true, false, true};
    case GeneratorPlan::ChunksOnly:
        return {true, true, false};
    case GeneratorPlan::Disabled:
        return {false, false, false};
    }
    return {false, false, false};
}

GeneratorPlan constrainPlan(GeneratorPlan client, GeneratorPlan server) {
    if (client == GeneratorPlan::Disabled || server == GeneratorPlan::Disabled) return GeneratorPlan::Disabled;
    const PlanFlags c = flagsOf(client), s = flagsOf(server);
    const bool      surface = c.surfaceGen && s.surfaceGen;
    const bool      chunk   = c.chunkGen && s.chunkGen;
    for (std::uint8_t i = 0; i < kGeneratorPlanCount; i++) {
        const auto      plan = static_cast<GeneratorPlan>(i);
        const PlanFlags f    = flagsOf(plan);
        if (f.generationEnabled && f.surfaceGen == surface && f.chunkGen == chunk) return plan;
    }
    return server;
}

std::int32_t constrainBandwidth(std::int32_t client, std::int32_t server) {
    if (client == 0 && server == 0) return 0;
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    return std::min(client > 0 ? client : kMax, server > 0 ? server : kMax);
}

} // namespace

void SessionConfigValues::encode(ByteWriter& w) const {
    NetCodec::putEnum(w, generatorPlan);
    w.putI32BE(maxGenerationRequestDistance);
    w.putI32BE(generationCenterChunkX);
    w.putI32BE(generationCenterChunkZ);
    w.putI32BE(generationMaxChunkRadius);
    w.putI32BE(generationRequestRateLimit);
    NetCodec::putBool(w, enableRealTimeUpdates);
    w.putI32BE(realTimeUpdateDistanceRadius);
    NetCodec::putBool(w, synchronizeOnLoad);
    w.putI32BE(maxSyncOnLoadRequestDistance);
    w.putI32BE(syncOnLoadRateLimit);
    w.putI32BE(playerBandwidthLimit);
}

bool SessionConfigValues::decode(ByteReader& r) {
    using NetCodec::getI32;
    return NetCodec::getEnum(r, generatorPlan, kGeneratorPlanCount) && getI32(r, maxGenerationRequestDistance)
        && getI32(r, generationCenterChunkX) && getI32(r, generationCenterChunkZ) && getI32(r, generationMaxChunkRadius)
        && getI32(r, generationRequestRateLimit) && NetCodec::getBool(r, enableRealTimeUpdates)
        && getI32(r, realTimeUpdateDistanceRadius) && NetCodec::getBool(r, synchronizeOnLoad)
        && getI32(r, maxSyncOnLoadRequestDistance) && getI32(r, syncOnLoadRateLimit) && getI32(r, playerBandwidthLimit);
}

SessionConfigValues constrain(const SessionConfigValues& c, const SessionConfigValues& s) {
    SessionConfigValues o;
    o.generatorPlan                = constrainPlan(c.generatorPlan, s.generatorPlan);
    o.maxGenerationRequestDistance = std::min(c.maxGenerationRequestDistance, s.maxGenerationRequestDistance);
    o.generationCenterChunkX       = s.generationCenterChunkX;
    o.generationCenterChunkZ       = s.generationCenterChunkZ;
    o.generationMaxChunkRadius     = s.generationMaxChunkRadius;
    o.generationRequestRateLimit   = std::min(c.generationRequestRateLimit, s.generationRequestRateLimit);
    o.enableRealTimeUpdates        = c.enableRealTimeUpdates && s.enableRealTimeUpdates;
    o.realTimeUpdateDistanceRadius = std::min(c.realTimeUpdateDistanceRadius, s.realTimeUpdateDistanceRadius);
    o.synchronizeOnLoad            = c.synchronizeOnLoad && s.synchronizeOnLoad;
    o.maxSyncOnLoadRequestDistance = std::min(c.maxSyncOnLoadRequestDistance, s.maxSyncOnLoadRequestDistance);
    o.syncOnLoadRateLimit          = std::min(c.syncOnLoadRateLimit, s.syncOnLoadRateLimit);
    o.playerBandwidthLimit         = constrainBandwidth(c.playerBandwidthLimit, s.playerBandwidthLimit);
    return o;
}

} // namespace dh::net
