#pragma once

// Read-mostly view of one 16x16 chunk column (Java: IChunkWrapper, subset the
// core actually consumes: LodDataBuilder, DhLightingEngine, chunk hashing).
//
// Coordinate conventions (identical to Java):
//   relX, relZ   chunk-relative, 0..15 inclusive. Out of range is a contract
//                violation (DH_DEBUG_ASSERT in implementations).
//   y            ABSOLUTE world Y, not chunk-relative. Valid block range is
//                [inclusiveMinBuildHeight(), exclusiveMaxBuildHeight()).
//                blockAt()/biomeAt() with y outside that range return the air
//                block / the implementation's default biome, never nullptr
//                (Java MC returns void-air there; the heightmap builder relies
//                on it by sampling y = maxNonEmptyHeight()).
//   chunkX/Z     chunk coordinates (block >> 4), may be negative.
//
// Height maps (Java ChunkWrapper.createDhHeightMaps, reproduced by TestChunk):
//   For each column scan down from y = maxNonEmptyHeight() while
//   y > minNonEmptyHeight(). solidHeightMapValue = first y with block.isSolid;
//   lightBlockingHeightMapValue = first y with block.opacity != 0 (includes
//   water). If none found both equal minNonEmptyHeight(). Note the block AT
//   minNonEmptyHeight() is never examined (Java loop condition) - keep.
//   There is no createDhHeightMaps() here: implementations compute lazily.
//
// Non-empty heights (Java ChunkWrapper): minNonEmptyHeight = min Y of the
// lowest 16-block section that contains a non-air block, or
// inclusiveMinBuildHeight if all empty; maxNonEmptyHeight = (min Y + 16) of the
// highest non-empty section. QUIRK: for an all-empty chunk Java's top-down loop
// assigns (sectionMin + 16) before testing and never breaks, so the result is
// inclusiveMinBuildHeight + 16 (NOT exclusiveMax as the Java doc comment
// claims). Implementations reproduce that; TestChunk does.
//
// DH lighting (Java ChunkLightStorage): 4-bit values 0..15 written by the DH
// lighting engine (WP-C2), separate from the game's own light.
//   skyLight:   y >= exclusiveMaxBuildHeight -> 15, y < inclusiveMin -> 0,
//               otherwise stored value (default 0).
//   blockLight: outside the build range -> 0, otherwise stored (default 0).
//   set*Light outside the build range is ignored. clear* resets stored values
//   to 0. The *LightingCorrect flags are owned by the lighting engine (false
//   until it has run; glue may start them true when native light is trusted).
//
// Lifetime/threading: an IChunk is a snapshot handed to one worker at a time;
// implementations need not be thread-safe. worldBlockLightPosList() is
// computed once and shared (Java: same note).

#include "dh/platform/BlockInfo.h"

#include <cstdint>
#include <span>

namespace dh {

/// A block that emits light, in WORLD block coordinates (Java getWorldBlockLightPosList).
struct BlockLightPos {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
    std::uint8_t level; ///< 1..15 (BlockInfo::lightEmission)
};

class IChunk {
public:
    static constexpr int kWidth = 16; ///< Java LodUtil.CHUNK_WIDTH

    virtual ~IChunk() = default;

    virtual std::int32_t chunkX() const = 0;
    virtual std::int32_t chunkZ() const = 0;

    std::int32_t minBlockX() const { return chunkX() * kWidth; }
    std::int32_t minBlockZ() const { return chunkZ() * kWidth; }

    virtual std::int32_t inclusiveMinBuildHeight() const = 0; ///< e.g. -64
    virtual std::int32_t exclusiveMaxBuildHeight() const = 0; ///< e.g. 320
    std::int32_t         height() const { return exclusiveMaxBuildHeight() - inclusiveMinBuildHeight(); }

    virtual std::int32_t minNonEmptyHeight() const = 0;
    virtual std::int32_t maxNonEmptyHeight() const = 0;

    /// Highest y with a solid block in the column (see header for exact rule).
    virtual std::int32_t solidHeightMapValue(int relX, int relZ) const = 0;
    /// Highest y with a non-transparent (opacity != 0) block; includes water.
    virtual std::int32_t lightBlockingHeightMapValue(int relX, int relZ) const = 0;

    /// Never nullptr; air for y outside the build range.
    virtual const BlockInfo* blockAt(int relX, std::int32_t y, int relZ) const = 0;
    /// Never nullptr; Java samples the biome at the top of the column
    /// (LodDataBuilder uses exclusiveMax-1 clamped), implementations may ignore y.
    virtual const BiomeInfo* biomeAt(int relX, std::int32_t y, int relZ) const = 0;

    // --- DH lighting -------------------------------------------------------
    virtual std::uint8_t dhSkyLight(int relX, std::int32_t y, int relZ) const                  = 0;
    virtual void         setDhSkyLight(int relX, std::int32_t y, int relZ, std::uint8_t level) = 0;
    virtual void         clearDhSkyLighting()                                                  = 0;

    virtual std::uint8_t dhBlockLight(int relX, std::int32_t y, int relZ) const                  = 0;
    virtual void         setDhBlockLight(int relX, std::int32_t y, int relZ, std::uint8_t level) = 0;
    virtual void         clearDhBlockLighting()                                                  = 0;

    virtual bool isDhBlockLightingCorrect() const = 0;
    virtual bool isDhSkyLightingCorrect() const   = 0;
    virtual void setDhBlockLightingCorrect(bool)  = 0;
    virtual void setDhSkyLightingCorrect(bool)    = 0;

    /// Light-emitting blocks in world coordinates. Stable for the chunk's lifetime.
    virtual std::span<const BlockLightPos> worldBlockLightPosList() const = 0;

    // --- helpers (non-virtual) -----------------------------------------------

    /// Java IChunkWrapper.getBlockBiomeHashCode ported with uint32 wrap-around
    /// and hashSerial() in place of Object.hashCode. Deterministic across
    /// runs/platforms (used by ChunkHashRepo to skip unchanged chunks); NOT
    /// equal to the Java value.
    std::uint32_t blockBiomeHashCode() const;

    /// Java relativeBlockPosToIndex: (zRel * 16 * height) + (yRel * 16) + xRel.
    std::int32_t relativeBlockPosToIndex(int relX, std::int32_t y, int relZ) const {
        std::int32_t yRel = y - inclusiveMinBuildHeight();
        return (relZ * kWidth * height()) + (yRel * kWidth) + relX;
    }

    static constexpr bool isValidRelXZ(int relX, int relZ) {
        return relX >= 0 && relX < kWidth && relZ >= 0 && relZ < kWidth;
    }
    bool isInBuildRange(std::int32_t y) const {
        return y >= inclusiveMinBuildHeight() && y < exclusiveMaxBuildHeight();
    }
};

} // namespace dh
