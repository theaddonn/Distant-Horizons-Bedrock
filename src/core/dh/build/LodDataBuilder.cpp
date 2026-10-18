#include "dh/build/LodDataBuilder.h"

#include <algorithm>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/pos/SectionPos.h"
#include "dh/util/Assert.h"

namespace dh {

namespace {

constexpr std::uint8_t MIN_MC_LIGHT        = 0;  // Java LodUtil.MIN_MC_LIGHT
constexpr std::uint8_t MAX_MC_LIGHT        = 15; // Java LodUtil.MAX_MC_LIGHT
constexpr int          NUMB_OF_CHUNKS_WIDE = 4;
constexpr int          BLOCK_FULLY_OPAQUE  = 16; // Java LodUtil.BLOCK_FULLY_OPAQUE

/// The MC wrapper's biome lookup clamps y into the chunk's section range; IChunk::biomeAt takes real y.
const BiomeInfo* biomeAtClamped(const IChunk& chunk, int x, std::int32_t y, int z) {
    const std::int32_t clamped = std::clamp(y, chunk.inclusiveMinBuildHeight(), chunk.exclusiveMaxBuildHeight() - 1);
    return chunk.biomeAt(x, clamped, z);
}

} // namespace

std::uint64_t sectionPosForChunk(std::int32_t chunkX, std::int32_t chunkZ) {
    return sec::encodeContainingChunk(sec::SECTION_BLOCK_DETAIL_LEVEL, chunkX, chunkZ);
}

int chunkOffsetInSection(std::int32_t chunkCoord) { return (chunkCoord & (NUMB_OF_CHUNKS_WIDE - 1)) * IChunk::kWidth; }

std::optional<FullDataSource> createFromChunk(const IChunk& chunk, const BuildOptions& options) {
    // :61 only block lighting is needed here
    DH_ASSERT(chunk.isDhBlockLightingCorrect(), "Provided chunk's DH Block lighting hasn't been baked.");

    const std::uint64_t pos = sectionPosForChunk(chunk.chunkX(), chunk.chunkZ());

    FullDataSource dataSource = FullDataSource::createEmpty(pos);
    dataSource.isEmpty        = false; // :74 (the later "mark non-empty" check at :235 is therefore dead)
    dataSource.applyToParent  = true;  // :76

    const int chunkOffsetX = chunkOffsetInSection(chunk.chunkX());
    const int chunkOffsetZ = chunkOffsetInSection(chunk.chunkZ());

    const std::int32_t minBuildHeight          = chunk.minNonEmptyHeight();
    const std::int32_t exclusiveMaxBuildHeight = chunk.exclusiveMaxBuildHeight();
    const std::int32_t inclusiveMinBuildHeight = chunk.inclusiveMinBuildHeight();
    const int          dataCapacity            = chunk.height() / 4;

    // IChunk::blockAt returns air outside the build range (Java AIR constant, :43).
    const BlockInfo* const air = chunk.blockAt(0, exclusiveMaxBuildHeight, 0);

    for (int relBlockX = 0; relBlockX < IChunk::kWidth; relBlockX++) {
        for (int relBlockZ = 0; relBlockZ < IChunk::kWidth; relBlockZ++) {
            const int columnX = relBlockX + chunkOffsetX;
            const int columnZ = relBlockZ + chunkOffsetZ;

            std::vector<std::uint64_t> longs;
            longs.reserve(static_cast<std::size_t>(std::max(dataCapacity, 0)));

            std::int32_t     lastY        = exclusiveMaxBuildHeight;                                    // :153
            const BiomeInfo* currentBiome = biomeAtClamped(chunk, relBlockX, lastY, relBlockZ);         // :154
            const BlockInfo* currentBlock = air;                                                        // :155
            int              mappedId     = dataSource.mapping.addIfAbsent(currentBiome, currentBlock); // :156

            // :159 height limit: no torches, sky not obscured
            std::uint8_t blockLight = MIN_MC_LIGHT;
            std::uint8_t skyLight   = MAX_MC_LIGHT;

            // :163-169
            std::int32_t y = std::max(
                chunk.lightBlockingHeightMapValue(relBlockX, relBlockZ),
                chunk.solidHeightMapValue(relBlockX, relBlockZ)
            );

            // :172-199 go up until open air or the world limit
            const BlockInfo* topBlock = chunk.blockAt(relBlockX, y, relBlockZ);
            while (!topBlock->isAir && y < exclusiveMaxBuildHeight) {
                y++;
                topBlock = chunk.blockAt(relBlockX, y, relBlockZ);
            }

            // :202 process blocks top to bottom
            bool forceSingleBlock = false;
            for (; y >= minBuildHeight; y--) {
                const BiomeInfo* newBiome = biomeAtClamped(chunk, relBlockX, y, relBlockZ);
                const BlockInfo* newBlock = chunk.blockAt(relBlockX, y, relBlockZ);
                // light of the block above (:206-207)
                const std::uint8_t newBlockLight = chunk.dhBlockLight(relBlockX, y + 1, relBlockZ);
                const std::uint8_t newSkyLight   = chunk.dhSkyLight(relBlockX, y + 1, relBlockZ);

                if (newBiome != currentBiome || newBlock != currentBlock || forceSingleBlock) { // :210-212
                    // :217-221 the previous block may colour this one
                    forceSingleBlock = !currentBlock->isAir && !currentBlock->isSolid && !currentBlock->isLiquid
                                    && currentBlock->opacity != BLOCK_FULLY_OPAQUE;

                    longs.push_back(
                        fdp::encode(mappedId, lastY - y, y + 1 - inclusiveMinBuildHeight, blockLight, skyLight)
                    );
                    currentBiome = newBiome;
                    currentBlock = newBlock;

                    mappedId   = dataSource.mapping.addIfAbsent(currentBiome, currentBlock);
                    blockLight = newBlockLight;
                    skyLight   = newSkyLight;
                    lastY      = y;
                }
            }

            // :259 final data point
            longs.push_back(fdp::encode(mappedId, lastY - y, y + 1 - inclusiveMinBuildHeight, blockLight, skyLight));

            // :262 setSingleColumn throws DataCorruptedException on a wrong-order column -> null (:266-270)
            if (FullDataSource::isColumnInWrongOrder(longs)) return std::nullopt;
            dataSource
                .setSingleColumn(std::move(longs), columnX, columnZ, GenerationStep::Light, options.worldCompression);
        }
    }

    return dataSource;
}

} // namespace dh
