#pragma once

// dh::IChunk over a Bedrock LevelChunk (read-only view; the LevelChunk must outlive the wrapper
// and must not be mutated concurrently - take the snapshot on the thread that owns the chunk).
//
// * blocks: the whole column range is materialised once (lazily) into a dense BlockInfo* array via
//   LevelChunk::getBlock(ChunkBlockPos) + BlockSerializer, so later queries are plain loads.
// * height maps / non-empty bounds: computed from BlockInfo flags with dh::chunkheight (Java
//   ChunkWrapper rules); Bedrock's mHeightmap is NOT used.
// * biomes: LevelChunk::getBiome per query (cached Biome* -> BiomeInfo*).
// * DH light: in-memory nibble-per-byte arrays, zero-initialised; isDh*Correct start false (the
//   native SubChunk light storages are not used, see docs/spikes/chunk-ingress.md).

#include "bedrock_common/BlockSerial.h"
#include "dh/platform/IChunk.h"

#include <cstdint>
#include <vector>

class LevelChunk;

namespace dh::bedrock {

class BedrockChunk final : public dh::IChunk {
public:
    /// False for dimensions whose height is not a positive multiple of 16 (callers skip those chunks).
    static bool heightSupported(LevelChunk& chunk);

    BedrockChunk(LevelChunk& chunk, BlockSerializer& serializer);

    std::int32_t chunkX() const override { return chunkX_; }
    std::int32_t chunkZ() const override { return chunkZ_; }
    std::int32_t inclusiveMinBuildHeight() const override { return minY_; }
    std::int32_t exclusiveMaxBuildHeight() const override { return maxY_; }
    std::int32_t minNonEmptyHeight() const override;
    std::int32_t maxNonEmptyHeight() const override;
    std::int32_t solidHeightMapValue(int relX, int relZ) const override;
    std::int32_t lightBlockingHeightMapValue(int relX, int relZ) const override;

    const dh::BlockInfo* blockAt(int relX, std::int32_t y, int relZ) const override;
    const dh::BiomeInfo* biomeAt(int relX, std::int32_t y, int relZ) const override;

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

    std::span<const dh::BlockLightPos> worldBlockLightPosList() const override;

private:
    std::size_t index(int relX, std::int32_t y, int relZ) const {
        return static_cast<std::size_t>(relativeBlockPosToIndex(relX, y, relZ));
    }
    void ensureBlocks() const;
    void computeBounds() const;
    void computeHeightMaps() const;

    LevelChunk&      chunk_;
    BlockSerializer& serializer_;
    std::int32_t     chunkX_, chunkZ_, minY_, maxY_;

    mutable std::vector<const dh::BlockInfo*> blocks_;
    std::vector<std::uint8_t>                 skyLight_;
    std::vector<std::uint8_t>                 blockLight_;
    bool                                      blockLightCorrect_ = false;
    bool                                      skyLightCorrect_   = false;

    mutable bool                           boundsValid_                    = false;
    mutable std::int32_t                   minNonEmpty_                    = 0;
    mutable std::int32_t                   maxNonEmpty_                    = 0;
    mutable bool                           heightMapsValid_                = false;
    mutable std::int32_t                   solidHeightMap_[16][16]         = {};
    mutable std::int32_t                   lightBlockingHeightMap_[16][16] = {};
    mutable bool                           lightListValid_                 = false;
    mutable std::vector<dh::BlockLightPos> lightList_;
};

} // namespace dh::bedrock
