#include "dh/build/LightingEngine.h"

#include "dh/fulldata/FullDataPoint.h"
#include "dh/platform/Direction.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dh {

namespace {

constexpr int kMaxLight = 15; // Java LodUtil.MAX_MC_LIGHT
constexpr int kMinLight = 0;  // Java LodUtil.MIN_MC_LIGHT

struct LightPos {
    std::int32_t x, y, z;
};

/// Java StableLightPosStack: one stack per light level.
struct LightQueue {
    std::array<std::vector<LightPos>, kMaxLight + 1> levels;

    void push(std::int32_t x, std::int32_t y, std::int32_t z, int level) {
        if (level < kMinLight || level > kMaxLight) {
            return;
        }
        levels[static_cast<std::size_t>(level)].push_back({x, y, z});
    }
};

/// Java AdjacentChunkHolder: 3x3 grid indexed 4 + offsetX + offsetZ * 3.
struct AdjacentChunks {
    std::array<IChunk*, 9> chunks{};

    explicit AdjacentChunks(IChunk& center) { chunks[4] = &center; }

    /// -1 when `chunk` is outside the 3x3.
    int slotOf(const IChunk& chunk) const {
        const IChunk& c  = *chunks[4];
        const int     ox = chunk.chunkX() - c.chunkX();
        const int     oz = chunk.chunkZ() - c.chunkZ();
        if (ox < -1 || ox > 1 || oz < -1 || oz > 1) {
            return -1;
        }
        return 4 + ox + oz * 3;
    }

    IChunk* byBlockPos(std::int32_t blockX, std::int32_t blockZ) const {
        const std::int32_t chunkX = blockX >> 4; // arithmetic shift == floor division
        const std::int32_t chunkZ = blockZ >> 4;
        const IChunk&      c      = *chunks[4];
        const int          ox     = chunkX - c.chunkX();
        const int          oz     = chunkZ - c.chunkZ();
        if (ox < -1 || ox > 1 || oz < -1 || oz > 1) {
            return nullptr;
        }
        return chunks[static_cast<std::size_t>(4 + ox + oz * 3)];
    }
};

template <typename GetFn, typename SetFn>
void propagate(LightQueue& queue, const AdjacentChunks& adjacent, GetFn getLight, SetFn setLight) {
    for (int level = kMaxLight; level >= kMinLight; level--) {
        auto& stack = queue.levels[static_cast<std::size_t>(level)];
        while (!stack.empty()) {
            const LightPos pos = stack.back(); // order is irrelevant, take the last element
            stack.pop_back();
            const int lightValue = level;

            for (Direction direction : kAllDirections) {
                const std::int32_t nx = pos.x + offsetX(direction);
                const std::int32_t ny = pos.y + offsetY(direction);
                const std::int32_t nz = pos.z + offsetZ(direction);

                IChunk* chunk = adjacent.byBlockPos(nx, nz);
                if (chunk == nullptr) {
                    continue; // outside the generator's range
                }
                if (ny < chunk->minNonEmptyHeight() || ny >= chunk->exclusiveMaxBuildHeight()) {
                    continue;
                }

                const int relX = nx & 15;
                const int relZ = nz & 15;

                const int current = getLight(*chunk, relX, ny, relZ);
                if (current >= (lightValue - 1)) {
                    continue; // already at least as bright as we could make it
                }

                const BlockInfo* block = chunk->blockAt(relX, ny, relZ);
                // drop by at least 1 so cycles terminate
                const int opacity = block != nullptr ? static_cast<int>(block->opacity) : 16;
                const int target  = lightValue - (opacity > 1 ? opacity : 1);
                if (target > current) {
                    setLight(*chunk, relX, ny, relZ, target);
                    queue.push(nx, ny, nz, target);
                }
            }
        }
    }
}

} // namespace

void lightChunk(IChunk& center, std::span<IChunk* const> nearby, const LightingOptions& options) {
    const bool updateBlockLight = options.generateBlockLight;
    const bool updateSkyLight   = options.generateSkyLight;
    const int  maxSkyLight      = options.maxSkyLight > kMaxLight ? kMaxLight : options.maxSkyLight;

    AdjacentChunks adjacent(center);
    LightQueue     blockQueue;
    LightQueue     skyQueue;

    auto seed = [&](IChunk& chunk) {
        // block lights
        if (updateBlockLight) {
            for (const BlockLightPos& p : chunk.worldBlockLightPosList()) {
                const int relX = p.x & 15;
                const int relZ = p.z & 15;
                // Java: lightValue = getLightEmission() of the block at the position
                const BlockInfo* block = chunk.blockAt(relX, p.y, relZ);
                const int        value = block != nullptr ? static_cast<int>(block->lightEmission) : 0;
                blockQueue.push(p.x, p.y, p.z, value);
                chunk.setDhBlockLight(relX, p.y, relZ, static_cast<std::uint8_t>(value));
            }
        }

        // sky lights: straight down each column until the first non-transparent block
        if (updateSkyLight && maxSkyLight > 0) {
            const std::int32_t maxY = chunk.maxNonEmptyHeight();
            const std::int32_t minY = chunk.inclusiveMinBuildHeight();
            for (int relX = 0; relX < IChunk::kWidth; relX++) {
                for (int relZ = 0; relZ < IChunk::kWidth; relZ++) {
                    for (std::int32_t y = maxY; y >= minY; y--) {
                        const BlockInfo* block = chunk.blockAt(relX, y, relZ);
                        if (block != nullptr && block->opacity != 0) {
                            break;
                        }
                        skyQueue.push(chunk.minBlockX() + relX, y, chunk.minBlockZ() + relZ, maxSkyLight);
                        chunk.setDhSkyLight(relX, y, relZ, static_cast<std::uint8_t>(maxSkyLight));
                    }
                }
            }
        }
    };

    bool seen[9] = {};

    // Java expects `center` inside the nearby list; treat it as implicitly present.
    seen[4] = true;
    seed(center);

    for (IChunk* chunk : nearby) {
        if (chunk == nullptr) {
            continue;
        }
        const int slot = adjacent.slotOf(*chunk);
        if (slot < 0 || seen[slot]) {
            continue; // outside the 3x3, or this position is already taken
        }
        seen[slot]                                      = true;
        adjacent.chunks[static_cast<std::size_t>(slot)] = chunk;
        seed(*chunk);
    }

    if (updateBlockLight) {
        // done to prevent a rare issue where the light values are incorrectly set to -1
        center.clearDhBlockLighting();
        propagate(
            blockQueue,
            adjacent,
            [](IChunk& c, int x, std::int32_t y, int z) { return static_cast<int>(c.dhBlockLight(x, y, z)); },
            [](IChunk& c, int x, std::int32_t y, int z, int v) {
                c.setDhBlockLight(x, y, z, static_cast<std::uint8_t>(v));
            }
        );
    }
    if (updateSkyLight) {
        center.clearDhSkyLighting();
        propagate(
            skyQueue,
            adjacent,
            [](IChunk& c, int x, std::int32_t y, int z) { return static_cast<int>(c.dhSkyLight(x, y, z)); },
            [](IChunk& c, int x, std::int32_t y, int z, int v) {
                c.setDhSkyLight(x, y, z, static_cast<std::uint8_t>(v));
            }
        );
    }

    if (updateBlockLight) {
        center.setDhBlockLightingCorrect(true);
    }
    if (updateSkyLight) {
        center.setDhSkyLightingCorrect(true);
    }
}

// ---------------------------------------------------------------------------
// data source sky light
// ---------------------------------------------------------------------------

namespace {

// Java ADJACENT_DIRECTION_OFFSETS: (-1,0) (+1,0) (0,-1) (0,+1)
constexpr int kAdjacentOffsets[8] = {-1, 0, +1, 0, 0, -1, 0, +1};

bool isAirId(const std::vector<bool>& airIds, int id) {
    return id >= 0 && static_cast<std::size_t>(id) < airIds.size() && airIds[static_cast<std::size_t>(id)];
}

/// Recursion depth is bounded by the light level (<= 15): each step lowers it by one.
void recursivelyLightAdjacent(
    FullDataSource&          source,
    const std::vector<bool>& airIds,
    int                      relX,
    int                      relZ,
    std::uint64_t            dataPoint
) {
    const int lightLevel = static_cast<int>(fdp::skyLight(dataPoint));
    if (lightLevel <= 1) {
        return; // adjacent light would be 0
    }

    const int minY = fdp::minY(dataPoint);
    const int maxY = fdp::height(dataPoint) + minY;

    for (int offsetIndex = 0; offsetIndex < 8;) {
        const int adjX = relX + kAdjacentOffsets[offsetIndex++];
        const int adjZ = relZ + kAdjacentOffsets[offsetIndex++];
        if (adjX < 0 || adjX >= FullDataSource::WIDTH || adjZ < 0 || adjZ >= FullDataSource::WIDTH) {
            continue;
        }
        std::vector<std::uint64_t>& adjColumn = source.column(adjX, adjZ);
        const std::size_t           size      = adjColumn.size();
        for (std::size_t adjIndex = 0; adjIndex < size; adjIndex++) {
            std::uint64_t adjPoint = adjColumn[adjIndex];
            const int     adjMinY  = fdp::minY(adjPoint);
            const int     adjMaxY  = fdp::height(adjPoint) + adjMinY;
            if (adjMinY >= maxY) {
                continue; // completely above this one
            }
            if (adjMaxY <= minY) {
                break; // completely below; so is everything after it (top-down)
            }
            if (!isAirId(airIds, fdp::id(adjPoint))) {
                continue;
            }
            const int adjLight = static_cast<int>(fdp::skyLight(adjPoint));
            if (lightLevel - 1 > adjLight) {
                adjPoint            = fdp::setSkyLight(adjPoint, lightLevel - 1);
                adjColumn[adjIndex] = adjPoint;
                recursivelyLightAdjacent(source, airIds, adjX, adjZ, adjPoint);
            }
        }
    }
}

} // namespace

void bakeDataSourceSkyLight(FullDataSource& dataSource, int maxSkyLight) {
    // clear old sky light
    for (auto& list : dataSource.columns) {
        for (std::uint64_t& data : list) {
            if (data != fdp::EMPTY_DATA_POINT) {
                data = fdp::setSkyLight(data, 0);
            }
        }
    }

    // "airIDs": ids whose block is completely transparent (opacity == 0), NOT isAir
    std::vector<bool> airIds(dataSource.mapping.size(), false);
    for (std::size_t id = 0; id < airIds.size(); id++) {
        const BlockInfo* block = dataSource.mapping.block(static_cast<int>(id));
        airIds[id]             = block != nullptr && block->opacity == 0;
    }

    for (int z = 0; z < FullDataSource::WIDTH; z++) {
        for (int x = 0; x < FullDataSource::WIDTH; x++) {
            std::vector<std::uint64_t>& points = dataSource.column(x, z);
            const std::size_t           size   = points.size();
            for (std::size_t index = 0; index < size; index++) {
                std::uint64_t point = points[index];
                if (!isAirId(airIds, fdp::id(point))) {
                    continue;
                }
                int skylight;
                if (index == 0) {
                    skylight = maxSkyLight; // top-most data point
                } else {
                    const std::uint64_t above = points[index - 1];
                    if (isAirId(airIds, fdp::id(above))) {
                        skylight = static_cast<int>(fdp::skyLight(above));
                    } else {
                        continue;
                    }
                }
                point         = fdp::setSkyLight(point, skylight);
                points[index] = point;
                recursivelyLightAdjacent(dataSource, airIds, x, z, point);
            }
        }
    }

    // opaque data points are lit from the one above
    for (auto& list : dataSource.columns) {
        for (std::size_t index = 0, size = list.size(); index < size; index++) {
            const std::uint64_t dataPoint = list[index];
            if (index == 0) {
                list[index] = fdp::setSkyLight(dataPoint, maxSkyLight);
            } else if (!isAirId(airIds, fdp::id(dataPoint))) {
                const std::uint64_t above      = list[index - 1];
                const int           aboveLight = static_cast<int>(fdp::skyLight(above));
                if (isAirId(airIds, fdp::id(above))) {
                    list[index] = fdp::setSkyLight(dataPoint, aboveLight);
                } else {
                    const BlockInfo* aboveBlock = dataSource.mapping.block(fdp::id(above));
                    const int        opacity    = aboveBlock != nullptr ? static_cast<int>(aboveBlock->opacity) : 16;
                    const int        absorption = opacity * fdp::height(above);
                    if (absorption < aboveLight) {
                        list[index] = fdp::setSkyLight(dataPoint, aboveLight - absorption);
                    }
                }
            }
        }
    }
}

} // namespace dh
