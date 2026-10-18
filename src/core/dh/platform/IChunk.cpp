#include "dh/platform/IChunk.h"

namespace dh {

namespace {
std::uint32_t blockHash(const BlockInfo* b) { return b ? hashSerial(b->serial) : 0u; }
std::uint32_t biomeHash(const BiomeInfo* b) { return b ? hashSerial(b->serial) : 0u; }
} // namespace

// Port of IChunkWrapper.getBlockBiomeHashCode (Java int arithmetic == uint32 wrap).
std::uint32_t IChunk::blockBiomeHashCode() const {
    std::uint32_t       hash                  = 31u;
    const std::uint32_t primeBlockMultiplier  = 227u;
    const std::uint32_t primeBiomeMultiplier  = 701u;
    const std::uint32_t primeHeightMultiplier = 137u;

    const std::int32_t minBuildHeight = minNonEmptyHeight();
    const std::int32_t maxBuildHeight = maxNonEmptyHeight();

    // every other block in the non-empty range
    for (int x = 0; x < kWidth; x += 2) {
        for (int z = 0; z < kWidth; z += 2) {
            for (std::int32_t y = minBuildHeight; y < maxBuildHeight; y += 2) {
                hash = (hash * primeBlockMultiplier) + blockHash(blockAt(x, y, z));
                hash = (hash * primeBiomeMultiplier) + biomeHash(biomeAt(x, y, z));
                hash = (hash * primeHeightMultiplier) + static_cast<std::uint32_t>(y);
            }
        }
    }

    // every surface block
    for (int x = 0; x < kWidth; x++) {
        for (int z = 0; z < kWidth; z++) {
            std::int32_t lightBlockingY = lightBlockingHeightMapValue(x, z);
            hash                        = (hash * primeBlockMultiplier) + blockHash(blockAt(x, lightBlockingY, z));
            hash                        = (hash * primeBiomeMultiplier) + biomeHash(biomeAt(x, lightBlockingY, z));
            hash                        = (hash * primeHeightMultiplier) + static_cast<std::uint32_t>(lightBlockingY);

            std::int32_t solidY = solidHeightMapValue(x, z);
            if (solidY != lightBlockingY) {
                // Java samples the block at lightBlockingY here (sic) but the biome at solidY; kept verbatim.
                hash = (hash * primeBlockMultiplier) + blockHash(blockAt(x, lightBlockingY, z));
                hash = (hash * primeBiomeMultiplier) + biomeHash(biomeAt(x, solidY, z));
                hash = (hash * primeHeightMultiplier) + static_cast<std::uint32_t>(solidY);
            }
        }
    }

    // light sources (world pos -> chunk relative: floorMod 16 == & 15)
    for (const BlockLightPos& pos : worldBlockLightPosList()) {
        int relX = pos.x & 15;
        int relZ = pos.z & 15;
        hash     = (hash * primeBlockMultiplier) + blockHash(blockAt(relX, pos.y, relZ));
        hash     = (hash * primeHeightMultiplier) + static_cast<std::uint32_t>(pos.y);
    }

    return hash;
}

} // namespace dh
