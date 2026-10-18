#pragma once

// Level (dimension) facts the core needs (Java: ILevelWrapper +
// IDimensionTypeWrapper, IClientLevelWrapper). Everything that touches live
// Minecraft state (chunk access, colours) stays behind this seam; the core only
// reads.
//
// Height convention: minHeight() inclusive, maxHeight() exclusive, like
// IChunk::inclusiveMinBuildHeight / exclusiveMaxBuildHeight. Bedrock overworld:
// -64 / 320.

#include "dh/platform/Direction.h"

#include <cstdint>
#include <string_view>

namespace dh {

struct BiomeInfo;
struct BlockInfo;

struct ILevelInfo {
    virtual ~ILevelInfo() = default;

    virtual std::int32_t minHeight() const = 0; ///< inclusive
    virtual std::int32_t maxHeight() const = 0; ///< exclusive

    virtual bool hasCeiling() const  = 0; ///< nether-like: skip sky light
    virtual bool hasSkyLight() const = 0;
    virtual bool isEnd() const       = 0;

    /// "minecraft:overworld" style; namespace + name (Java getDimensionName).
    virtual std::string_view dimensionName() const = 0;

    /// Stable string that uniquely identifies this level for storage paths and
    /// network LevelInit (Java getDhIdentifier). Defaults to dimensionName().
    virtual std::string_view dhIdentifier() const { return dimensionName(); }

    /// Java getHashedSeed; 0 if the platform cannot provide one.
    virtual std::int64_t hashedSeed() const { return 0; }

    std::int32_t height() const { return maxHeight() - minHeight(); }
};

/// Client-side level: adds colour sampling (Java IClientLevelWrapper).
struct IClientLevel : ILevelInfo {
    /// Colour of `block` at the given world block position (tinted by biome for
    /// grass/foliage/water), 0xAARRGGBB. Alpha carries block transparency.
    /// Called from worker threads; implementations cache aggressively.
    /// Java: getBlockColor(pos, biome, fullDataSource, blockState); the
    /// fullDataSource argument was only used for API overrides and is dropped.
    virtual std::uint32_t blockColor(
        std::int32_t     blockX,
        std::int32_t     blockY,
        std::int32_t     blockZ,
        const BiomeInfo* biome,
        const BlockInfo* block
    ) = 0;

    /// Java getDirtBlockColor; returns 0xFFFFFFFF (-1) if unavailable.
    virtual std::uint32_t dirtBlockColor() = 0;

    /// Face shade multiplier in [0,1] (Java getShade: Up 1.0, Down 0.5,
    /// North/South 0.8, West/East 0.6 in vanilla).
    virtual float shade(Direction face) const = 0;

    /// Drop cached colours (resource pack reload). Default no-op.
    virtual void clearBlockColorCache() {}
};

} // namespace dh
