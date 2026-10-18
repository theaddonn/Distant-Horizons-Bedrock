#include "bedrock_common/BedrockChunk.h"

#include "dh/platform/ChunkHeightUtil.h"
#include "dh/util/Assert.h"

#include "mc/world/level/ChunkBlockPos.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"

namespace dh::bedrock {

BedrockChunk::BedrockChunk(LevelChunk& chunk, BlockSerializer& serializer)
: chunk_(chunk),
  serializer_(serializer),
  chunkX_(chunk.mPosition->x),
  chunkZ_(chunk.mPosition->z),
  minY_(chunk.mDimension.mHeightRange->mMin),
  maxY_(chunk.mDimension.mHeightRange->mMax) {}

bool BedrockChunk::heightSupported(LevelChunk& chunk) {
    const int lo = chunk.mDimension.mHeightRange->mMin;
    const int hi = chunk.mDimension.mHeightRange->mMax;
    return hi > lo && (hi - lo) % 16 == 0;
}

void BedrockChunk::ensureBlocks() const {
    if (!blocks_.empty()) return;
    blocks_.assign(
        static_cast<std::size_t>(16) * 16 * static_cast<std::size_t>(maxY_ - minY_),
        serializer_.table().air()
    );
    const Block*         lastBlock = nullptr;
    const dh::BlockInfo* lastInfo  = nullptr;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            for (std::int32_t y = minY_; y < maxY_; ++y) {
                ChunkBlockPos pos{
                    static_cast<uchar>(x),
                    ChunkLocalHeight{static_cast<short>(y - minY_)},
                    static_cast<uchar>(z)
                };
                const Block& b = chunk_.getBlock(pos);
                if (&b != lastBlock) {
                    lastBlock = &b;
                    lastInfo  = serializer_.resolve(b);
                }
                blocks_[index(x, y, z)] = lastInfo;
            }
        }
    }
}

const dh::BlockInfo* BedrockChunk::blockAt(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return serializer_.table().air();
    ensureBlocks();
    return blocks_[index(relX, y, relZ)];
}

const dh::BiomeInfo* BedrockChunk::biomeAt(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (y < minY_) y = minY_;
    if (y >= maxY_) y = maxY_ - 1;
    ChunkBlockPos pos{
        static_cast<uchar>(relX),
        ChunkLocalHeight{static_cast<short>(y - minY_)},
        static_cast<uchar>(relZ)
    };
    return serializer_.resolve(chunk_.getBiome(pos));
}

// --- bounds / height maps ---------------------------------------------------

void BedrockChunk::computeBounds() const {
    ensureBlocks();
    auto sectionEmpty = [&](int section) {
        const std::int32_t base = minY_ + section * 16;
        for (std::int32_t y = base; y < base + 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    if (!blocks_[index(x, y, z)]->isAir) return false;
        return true;
    };
    const auto b = chunkheight::computeNonEmptyBounds(minY_, maxY_, sectionEmpty);
    minNonEmpty_ = b.minNonEmpty;
    maxNonEmpty_ = b.maxNonEmpty;
    boundsValid_ = true;
}

std::int32_t BedrockChunk::minNonEmptyHeight() const {
    if (!boundsValid_) computeBounds();
    return minNonEmpty_;
}

std::int32_t BedrockChunk::maxNonEmptyHeight() const {
    if (!boundsValid_) computeBounds();
    return maxNonEmpty_;
}

void BedrockChunk::computeHeightMaps() const {
    chunkheight::computeHeightMaps(
        minNonEmptyHeight(),
        maxNonEmptyHeight(),
        [&](int x, std::int32_t y, int z) { return blockAt(x, y, z); },
        solidHeightMap_,
        lightBlockingHeightMap_
    );
    heightMapsValid_ = true;
}

std::int32_t BedrockChunk::solidHeightMapValue(int relX, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!heightMapsValid_) computeHeightMaps();
    return solidHeightMap_[relX][relZ];
}

std::int32_t BedrockChunk::lightBlockingHeightMapValue(int relX, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!heightMapsValid_) computeHeightMaps();
    return lightBlockingHeightMap_[relX][relZ];
}

// --- DH light ---------------------------------------------------------------

std::uint8_t BedrockChunk::dhSkyLight(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (y < minY_) return 0;
    if (y >= maxY_) return 15;
    if (skyLight_.empty()) return 0;
    return skyLight_[index(relX, y, relZ)];
}

void BedrockChunk::setDhSkyLight(int relX, std::int32_t y, int relZ, std::uint8_t level) {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return;
    if (skyLight_.empty()) skyLight_.assign(static_cast<std::size_t>(16) * 16 * (maxY_ - minY_), 0);
    skyLight_[index(relX, y, relZ)] = level & 15;
}

void BedrockChunk::clearDhSkyLighting() { skyLight_.assign(skyLight_.size(), 0); }

std::uint8_t BedrockChunk::dhBlockLight(int relX, std::int32_t y, int relZ) const {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y) || blockLight_.empty()) return 0;
    return blockLight_[index(relX, y, relZ)];
}

void BedrockChunk::setDhBlockLight(int relX, std::int32_t y, int relZ, std::uint8_t level) {
    DH_DEBUG_ASSERT(isValidRelXZ(relX, relZ), "relX/relZ out of range");
    if (!isInBuildRange(y)) return;
    if (blockLight_.empty()) blockLight_.assign(static_cast<std::size_t>(16) * 16 * (maxY_ - minY_), 0);
    blockLight_[index(relX, y, relZ)] = level & 15;
}

void BedrockChunk::clearDhBlockLighting() { blockLight_.assign(blockLight_.size(), 0); }

std::span<const dh::BlockLightPos> BedrockChunk::worldBlockLightPosList() const {
    if (!lightListValid_) {
        ensureBlocks();
        lightList_.clear();
        for (std::int32_t y = minY_; y < maxY_; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    const dh::BlockInfo* b = blocks_[index(x, y, z)];
                    if (b->lightEmission > 0)
                        lightList_.push_back(dh::BlockLightPos{minBlockX() + x, y, minBlockZ() + z, b->lightEmission});
                }
        lightListValid_ = true;
    }
    return lightList_;
}

} // namespace dh::bedrock
