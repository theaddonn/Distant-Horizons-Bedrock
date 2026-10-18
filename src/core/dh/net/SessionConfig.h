#pragma once

// Port of multiplayer/config/SessionConfig.java (values + constrain rules + wire encoding).
// Wire order is the Java registration order; each value is encoded with the Java Codec for its type
// (enum = 1 byte ordinal, int = i32 BE, bool = 1 byte), no names or count.

#include "dh/net/Message.h"

#include <cstdint>

namespace dh::net {

/// EDhApiGeneratorPlan; ordinal order is wire order.
enum class GeneratorPlan : std::uint8_t {
    SurfaceThenChunks = 0,
    SurfaceOnly       = 1,
    ChunksOnly        = 2,
    Disabled          = 3,
};
inline constexpr std::uint8_t kGeneratorPlanCount = 4;

struct SessionConfigValues {
    GeneratorPlan generatorPlan                = GeneratorPlan::SurfaceThenChunks; // generation.plan
    std::int32_t  maxGenerationRequestDistance = 4096;                             // generation.maxRequestDistance
    std::int32_t  generationCenterChunkX       = 0;                                // generation.bounds.centerChunk.x
    std::int32_t  generationCenterChunkZ       = 0;                                // generation.bounds.centerChunk.z
    std::int32_t  generationMaxChunkRadius     = 0;                                // generation.bounds.radiusInChunks
    std::int32_t  generationRequestRateLimit   = 20;                               // generation.requestRateLimit
    bool          enableRealTimeUpdates        = true;                             // realTimeUpdates.enable
    std::int32_t  realTimeUpdateDistanceRadius = 256;                              // realTimeUpdates.playerDistance
    bool          synchronizeOnLoad            = true;                             // syncOnLoad.enable
    std::int32_t  maxSyncOnLoadRequestDistance = 4096;                             // syncOnLoad.maxRequestDistance
    std::int32_t  syncOnLoadRateLimit          = 50;                               // syncOnLoad.rateLimit
    std::int32_t  playerBandwidthLimit         = 500; // common.playerBandwidthLimit (KB/s, 0 = unlimited)

    friend bool operator==(const SessionConfigValues&, const SessionConfigValues&) = default;

    void encode(ByteWriter& w) const;
    bool decode(ByteReader& r);
};

/// Java SessionConfig entry constrainers applied as f(client, server):
///  - generatorPlan: Disabled if either is Disabled; else first plan (enum order) with generation enabled
///    whose surface/chunk flags equal (client && server); none matches -> server's plan
///  - distances / rate limits / realtime radius: min
///  - generationCenterChunkX/Z, generationMaxChunkRadius: server's value
///  - booleans: logical and
///  - playerBandwidthLimit: both 0 -> 0, else min of the positive ones (0 = unlimited)
SessionConfigValues constrain(const SessionConfigValues& client, const SessionConfigValues& server);

} // namespace dh::net
