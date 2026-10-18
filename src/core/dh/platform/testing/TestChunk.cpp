#include "dh/platform/testing/TestChunk.h"

#include "dh/util/Assert.h"

namespace dh {

TestChunk::TestChunk(
    std::int32_t     chunkX,
    std::int32_t     chunkZ,
    std::int32_t     minY,
    std::int32_t     maxY,
    const BlockInfo* air,
    const BiomeInfo* biome
)
: chunkX_(chunkX),
  chunkZ_(chunkZ),
  minY_(minY),
  maxY_(maxY),
  air_(air) {
    DH_ASSERT(maxY > minY && (maxY - minY) % 16 == 0, "TestChunk height must be a positive multiple of 16");
    DH_ASSERT(air && air->isAir, "TestChunk needs an air block");
    DH_ASSERT(biome, "TestChunk needs a default biome");
    std::size_t n = static_cast<std::size_t>(kWidth) * kWidth * static_cast<std::size_t>(maxY - minY);
    blocks_.assign(n, air);
    biomes_.assign(n, biome);
    skyLight_.assign(n, 0);
    blockLight_.assign(n, 0);
}

// --- mutation ---------------------------------------------------------------

void TestChunk::setBlock(int relX, std::int32_t y, int relZ, const BlockInfo* block) {
    DH_ASSERT(isValidRelXZ(relX, relZ) && isInBuildRange(y), "TestChunk::setBlock out of range");
    DH_ASSERT(block, "TestChunk::setBlock null block");
    blocks_[index(relX, y, relZ)] = block;
    invalidate();
}

void TestChunk::setBiome(int relX, std::int32_t y, int relZ, const BiomeInfo* biome) {
    DH_ASSERT(isValidRelXZ(relX, relZ) && isInBuildRange(y), "TestChunk::setBiome out of range");
    DH_ASSERT(biome, "TestChunk::setBiome null biome");
    biomes_[index(relX, y, relZ)] = biome;
}

void TestChunk::fillColumn(int relX, int relZ, std::int32_t yFrom, std::int32_t yToExclusive, const BlockInfo* block) {
    for (std::int32_t y = yFrom; y < yToExclusive; ++y) setBlock(relX, y, relZ, block);
}

void TestChunk::fillLayer(std::int32_t y, const BlockInfo* block) {
    for (int x = 0; x < kWidth; ++x)
        for (int z = 0; z < kWidth; ++z) setBlock(x, y, z, block);
}

void TestChunk::fillLayers(std::int32_t yFrom, std::int32_t yToExclusive, const BlockInfo* block) {
    for (std::int32_t y = yFrom; y < yToExclusive; ++y) fillLayer(y, block);
}

void TestChunk::fillBiome(const BiomeInfo* biome) {
    DH_ASSERT(biome, "TestChunk::fillBiome null biome");
    for (auto& b : biomes_) b = biome;
}

// --- bounds / height maps (Java ChunkWrapper semantics) ---------------------

void TestChunk::computeBounds() const {
    const int sectionCount = (maxY_ - minY_) / 16;
    auto      sectionEmpty = [&](int section) {
        std::int32_t base = minY_ + section * 16;
        for (std::int32_t y = base; y < base + 16; ++y)
            for (int z = 0; z < kWidth; ++z)
                for (int x = 0; x < kWidth; ++x)
                    if (!blocks_[index(x, y, z)]->isAir) return false;
        return true;
    };

    // Java getMinNonEmptyHeight: bottom-up, first non-empty section's min Y.
    minNonEmpty_ = minY_;
    for (int s = 0; s < sectionCount; ++s) {
        if (!sectionEmpty(s)) {
            minNonEmpty_ = minY_ + s * 16;
            break;
        }
    }

    // Java getMaxNonEmptyHeight: top-down, value updated at each section to
    // (section min + 16) before the emptiness check; stays exclusiveMax if all empty.
    maxNonEmpty_ = maxY_;
    for (int s = sectionCount - 1; s >= 0; --s) {
        maxNonEmpty_ = minY_ + s * 16 + 16;
        if (!sectionEmpty(s)) break;
    }
    boundsValid_ = true;
}

std::int32_t TestChunk::minNonEmptyHeight() const {
    if (!boundsValid_) computeBounds();
    return minNonEmpty_;
}

std::int32_t TestChunk::maxNonEmptyHeight() const {
    if (!boundsValid_) computeBounds();
    return maxNonEmpty_;
}

void TestChunk::computeHeightMaps() const {
    // Verbatim port of ChunkWrapper.createDhHeightMaps.
    const std::int32_t minInclusiveBuildHeight = minNonEmptyHeight();
    for (int x = 0; x < kWidth; x++) {
        for (int z = 0; z < kWidth; z++) {
            std::int32_t solidHeight         = minInclusiveBuildHeight;
            std::int32_t lightBlockingHeight = minInclusiveBuildHeight;

            std::int32_t     y     = maxNonEmptyHeight();
            const BlockInfo* block = blockAt(x, y, z);
            while (y > minInclusiveBuildHeight
                   && (solidHeight == minInclusiveBuildHeight || lightBlockingHeight == minInclusiveBuildHeight)) {
                if (solidHeight == minInclusiveBuildHeight && block->isSolid) solidHeight = y;
                if (lightBlockingHeight == minInclusiveBuildHeight && block->opacity != 0) lightBlockingHeight = y;
                y--;
                block = blockAt(x, y, z);
            }

            solidHeightMap_[x][z]         = solidHeight;
            lightBlockingHeightMap_[x][z] = lightBlockingHeight;
        }
    }
    heightMapsValid_ = true;
}

std::int32_t TestChunk::solidHeightMapValue(int relX, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!heightMapsValid_) computeHeightMaps();
    return solidHeightMap_[relX][relZ];
}

std::int32_t TestChunk::lightBlockingHeightMapValue(int relX, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!heightMapsValid_) computeHeightMaps();
    return lightBlockingHeightMap_[relX][relZ];
}

// --- blocks / biomes --------------------------------------------------------

const BlockInfo* TestChunk::blockAt(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return air_;
    return blocks_[index(relX, y, relZ)];
}

const BiomeInfo* TestChunk::biomeAt(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (y < minY_) y = minY_;
    if (y >= maxY_) y = maxY_ - 1;
    return biomes_[index(relX, y, relZ)];
}

// --- DH lighting (Java ChunkLightStorage semantics) -------------------------

std::uint8_t TestChunk::dhSkyLight(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (y < minY_) return 0;
    if (y >= maxY_) return 15;
    return skyLight_[index(relX, y, relZ)];
}

void TestChunk::setDhSkyLight(int relX, std::int32_t y, int relZ, std::uint8_t level) {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return;
    skyLight_[index(relX, y, relZ)] = level & 15;
}

void TestChunk::clearDhSkyLighting() { skyLight_.assign(skyLight_.size(), 0); }

std::uint8_t TestChunk::dhBlockLight(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return 0;
    return blockLight_[index(relX, y, relZ)];
}

void TestChunk::setDhBlockLight(int relX, std::int32_t y, int relZ, std::uint8_t level) {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return;
    blockLight_[index(relX, y, relZ)] = level & 15;
}

void TestChunk::clearDhBlockLighting() { blockLight_.assign(blockLight_.size(), 0); }

std::span<const BlockLightPos> TestChunk::worldBlockLightPosList() const {
    if (!lightListValid_) {
        lightList_.clear();
        for (std::int32_t y = minY_; y < maxY_; ++y)
            for (int z = 0; z < kWidth; ++z)
                for (int x = 0; x < kWidth; ++x) {
                    const BlockInfo* b = blocks_[index(x, y, z)];
                    if (b->lightEmission > 0)
                        lightList_.push_back(BlockLightPos{minBlockX() + x, y, minBlockZ() + z, b->lightEmission});
                }
        lightListValid_ = true;
    }
    return lightList_;
}

} // namespace dh
