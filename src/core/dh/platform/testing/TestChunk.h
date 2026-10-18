#pragma once

// In-memory IChunk for unit tests, golden fixtures and host tools. Dense
// 16 x height x 16 storage; height maps and non-empty bounds are computed
// lazily with the exact Java ChunkWrapper rules (see IChunk.h) and invalidated
// on every block change.

#include "dh/platform/IChunk.h"

#include <cstdint>
#include <span>
#include <vector>

namespace dh {

class TestChunk final : public IChunk {
public:
    /// `minY` inclusive, `maxY` exclusive, (maxY - minY) must be a multiple of 16.
    /// Every block starts as `air` (must have isAir), every biome as `biome`.
    TestChunk(
        std::int32_t     chunkX,
        std::int32_t     chunkZ,
        std::int32_t     minY,
        std::int32_t     maxY,
        const BlockInfo* air,
        const BiomeInfo* biome
    );

    // --- mutation (tests only) ----------------------------------------------
    void setBlock(int relX, std::int32_t y, int relZ, const BlockInfo* block);
    void setBiome(int relX, std::int32_t y, int relZ, const BiomeInfo* biome);
    /// Sets [yFrom, yToExclusive) in one column.
    void fillColumn(int relX, int relZ, std::int32_t yFrom, std::int32_t yToExclusive, const BlockInfo* block);
    /// Sets the whole 16x16 layer at `y`.
    void fillLayer(std::int32_t y, const BlockInfo* block);
    /// Sets every block in [yFrom, yToExclusive) across the chunk.
    void fillLayers(std::int32_t yFrom, std::int32_t yToExclusive, const BlockInfo* block);
    /// Sets the biome for the entire chunk.
    void fillBiome(const BiomeInfo* biome);

    // --- IChunk --------------------------------------------------------------
    std::int32_t chunkX() const override { return chunkX_; }
    std::int32_t chunkZ() const override { return chunkZ_; }
    std::int32_t inclusiveMinBuildHeight() const override { return minY_; }
    std::int32_t exclusiveMaxBuildHeight() const override { return maxY_; }
    std::int32_t minNonEmptyHeight() const override;
    std::int32_t maxNonEmptyHeight() const override;
    std::int32_t solidHeightMapValue(int relX, int relZ) const override;
    std::int32_t lightBlockingHeightMapValue(int relX, int relZ) const override;

    const BlockInfo* blockAt(int relX, std::int32_t y, int relZ) const override;
    const BiomeInfo* biomeAt(int relX, std::int32_t y, int relZ) const override;

    std::uint8_t dhSkyLight(int relX, std::int32_t y, int relZ) const override;
    void         setDhSkyLight(int relX, std::int32_t y, int relZ, std::uint8_t level) override;
    void         clearDhSkyLighting() override;
    std::uint8_t dhBlockLight(int relX, std::int32_t y, int relZ) const override;
    void         setDhBlockLight(int relX, std::int32_t y, int relZ, std::uint8_t level) override;
    void         clearDhBlockLighting() override;

    bool isDhBlockLightingCorrect() const override { return blockLightCorrect_; }
    bool isDhSkyLightingCorrect() const override { return skyLightCorrect_; }
    void setDhBlockLightingCorrect(bool v) override { blockLightCorrect_ = v; }
    void setDhSkyLightingCorrect(bool v) override { skyLightCorrect_ = v; }

    /// Blocks with lightEmission > 0, ordered by (y, z, x) ascending; world coordinates.
    std::span<const BlockLightPos> worldBlockLightPosList() const override;

private:
    std::size_t index(int relX, std::int32_t y, int relZ) const {
        return static_cast<std::size_t>(relativeBlockPosToIndex(relX, y, relZ));
    }
    void invalidate() {
        heightMapsValid_ = false;
        boundsValid_     = false;
        lightListValid_  = false;
    }
    void computeBounds() const;
    void computeHeightMaps() const;

    std::int32_t     chunkX_, chunkZ_, minY_, maxY_;
    const BlockInfo* air_;

    std::vector<const BlockInfo*> blocks_;
    std::vector<const BiomeInfo*> biomes_;
    std::vector<std::uint8_t>     skyLight_;
    std::vector<std::uint8_t>     blockLight_;
    bool                          blockLightCorrect_ = false;
    bool                          skyLightCorrect_   = false;

    // lazily derived
    mutable bool                       boundsValid_                            = false;
    mutable std::int32_t               minNonEmpty_                            = 0;
    mutable std::int32_t               maxNonEmpty_                            = 0;
    mutable bool                       heightMapsValid_                        = false;
    mutable std::int32_t               solidHeightMap_[kWidth][kWidth]         = {};
    mutable std::int32_t               lightBlockingHeightMap_[kWidth][kWidth] = {};
    mutable bool                       lightListValid_                         = false;
    mutable std::vector<BlockLightPos> lightList_;
};

} // namespace dh
