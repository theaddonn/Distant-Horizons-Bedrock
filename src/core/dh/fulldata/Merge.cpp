#include "dh/fulldata/Merge.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/OcclusionCuller.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Bits.h"

namespace dh {

namespace {

constexpr int WIDTH = FullDataSource::WIDTH;

using Column = std::vector<std::uint64_t>;
using Steps  = std::array<std::int8_t, FullDataSource::COLUMN_COUNT>;

/// Java FullDataPointUtil.remap. Java throws on an out-of-range id; here the datapoint is left unchanged.
std::uint64_t remapPoint(std::uint64_t d, std::span<const int> remap) {
    const int id = fdp::id(d);
    if (id < 0 || static_cast<std::size_t>(id) >= remap.size()) return d;
    return fdp::setId(d, remap[static_cast<std::size_t>(id)]);
}

void remapDataColumn(FullDataSource& self, int index, std::span<const int> remap) {
    for (std::uint64_t& d : self.columns[static_cast<std::size_t>(index)]) d = remapPoint(d, remap);
}

/// Java FullDataPointUtil.validateData; Java only runs it in dev builds, the merge catches the throw and skips.
bool isValidDatapoint(int id, int height, int relMinY, int blockLight, int skyLight) {
    constexpr int maxY = rdp::MAX_WORLD_Y_SIZE;
    if (id < 0) return false;
    if (relMinY < 0 || relMinY >= maxY) return false;
    if (height <= 0 || height >= maxY) return false;
    if (relMinY + height > maxY) return false;
    if (blockLight < 0 || blockLight > 15) return false;
    if (skyLight < 0 || skyLight > 15) return false;
    return true;
}

std::int8_t minGenStepOfTwoByTwo(const Steps& steps, int relX, int relZ) {
    std::int8_t m = INT8_MAX;
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++)
            m = std::min(m, steps[static_cast<std::size_t>(FullDataSource::relativePosToIndex(x + relX, z + relZ))]);
    return m;
}

std::int8_t maxCompressionOfTwoByTwo(const Steps& modes, int relX, int relZ) {
    std::int8_t m = INT8_MIN;
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++)
            m = std::max(m, modes[static_cast<std::size_t>(FullDataSource::relativePosToIndex(x + relX, z + relZ))]);
    return m;
}

bool isAir(const IdMap& mapping, int id) {
    const BlockInfo* b = mapping.block(id);
    return b != nullptr && b->isAir;
}

/// Java determineMostCommonValueInColumnSlice (mapping is never null in the only caller).
int determineMostCommonValueInColumnSlice(const std::array<int, 4>& s, const IdMap& mapping) {
    const int value0 = s[0], value1 = s[1], value2 = s[2], value3 = s[3];
    int       count0 = 0, count1 = 0, count2 = 0, count3 = 0;

    for (int i = 0; i < 4; i++) {
        const int value = s[static_cast<std::size_t>(i)];
        if (isAir(mapping, value)) continue; // air never wins
        if (value == value0) count0++;
        else if (value == value1) count1++;
        else if (value == value2) count2++;
        else count3++;
    }

    const int maxCount = std::max(count0, std::max(count1, std::max(count2, count3)));
    if (maxCount == count0) return value0;
    if (maxCount == count1) return value1;
    if (maxCount == count2) return value2;
    return value3;
}

int determineAverageValueInColumnSlice(const std::array<int, 4>& s) {
    int value = 0;
    for (int i = 0; i < 4; i++) value += s[static_cast<std::size_t>(i)];
    return jdiv(value, 4);
}

int determineAverageValueInColumnSliceWithId(
    const std::array<int, 4>& s,
    const std::array<int, 4>& mergeIds,
    int                       winningId
) {
    int sum = 0, count = 0;
    for (int i = 0; i < 4; i++) {
        if (mergeIds[static_cast<std::size_t>(i)] == winningId) {
            sum += s[static_cast<std::size_t>(i)];
            count++;
        }
    }
    if (count == 0) return determineAverageValueInColumnSlice(s);
    return jdiv(sum, count);
}

Column mergeInputTwoByTwoDataColumn(const FullDataSource& input, int x, int z) {
    Column newColumn;

    std::array<const Column*, 4> inputColumns{};
    int                          colIndex = 0;
    for (int inputX = x; inputX < x + 2; inputX++)
        for (int inputZ = z; inputZ < z + 2; inputZ++, colIndex++)
            inputColumns[static_cast<std::size_t>(colIndex)] = &input.column(inputX, inputZ);

    // all y levels where something changes (contains-deduped, then sorted ascending)
    std::vector<int> yTransitions;
    for (int i = 0; i < 4; i++) {
        for (std::uint64_t datapoint : *inputColumns[static_cast<std::size_t>(i)]) {
            const int minY = fdp::minY(datapoint);
            const int maxY = minY + fdp::height(datapoint);
            if (std::find(yTransitions.begin(), yTransitions.end(), minY) == yTransitions.end())
                yTransitions.push_back(minY);
            if (std::find(yTransitions.begin(), yTransitions.end(), maxY) == yTransitions.end())
                yTransitions.push_back(maxY);
        }
    }
    if (yTransitions.empty()) return newColumn;
    std::sort(yTransitions.begin(), yTransitions.end());

    // per column, start at the lowest datapoint (last element); -1 = none
    std::array<int, 4> currentIndices{};
    for (std::size_t i = 0; i < 4; i++) {
        const Column& c   = *inputColumns[i];
        currentIndices[i] = c.empty() ? -1 : static_cast<int>(c.size()) - 1;
    }

    int          lastId            = 0;
    std::uint8_t lastBlockLight    = 0;
    std::uint8_t lastSkyLight      = 0;
    int          currentMinY       = yTransitions[0];
    int          accumulatedHeight = 0;

    auto emit = [&]() {
        if (isValidDatapoint(lastId, accumulatedHeight, currentMinY, lastBlockLight, lastSkyLight)) {
            newColumn.push_back(fdp::encode(lastId, accumulatedHeight, currentMinY, lastBlockLight, lastSkyLight));
        }
        // else: Java logs "Skipping corrupt datapoint" and drops it
    };

    std::array<int, 4> mergeIds{}, mergeBlockLights{}, mergeSkyLights{};
    for (std::size_t yIndex = 0; yIndex + 1 < yTransitions.size(); yIndex++) {
        const int sliceMinY   = yTransitions[yIndex];
        const int sliceMaxY   = yTransitions[yIndex + 1];
        const int sliceHeight = sliceMaxY - sliceMinY;
        const int sampleY     = sliceMinY + jdiv(sliceHeight, 2);

        mergeIds.fill(0);
        mergeBlockLights.fill(0);
        mergeSkyLights.fill(0);

        for (std::size_t i = 0; i < 4; i++) {
            if (currentIndices[i] == -1) continue;
            const Column& column = *inputColumns[i];
            while (currentIndices[i] >= 0) {
                const std::uint64_t datapoint = column[static_cast<std::size_t>(currentIndices[i])];
                const int           inputMinY = fdp::minY(datapoint);
                const int           inputMaxY = inputMinY + fdp::height(datapoint);
                if (sampleY >= inputMaxY) {
                    currentIndices[i]--;
                } else if (sampleY >= inputMinY && sampleY < inputMaxY) {
                    mergeIds[i]         = fdp::id(datapoint);
                    mergeBlockLights[i] = fdp::blockLight(datapoint);
                    mergeSkyLights[i]   = fdp::skyLight(datapoint);
                    break;
                } else {
                    break;
                }
            }
        }

        const int id = determineMostCommonValueInColumnSlice(mergeIds, input.mapping);
        // light is only averaged over the sub-columns that voted for the winner
        const auto blockLight =
            static_cast<std::uint8_t>(determineAverageValueInColumnSliceWithId(mergeBlockLights, mergeIds, id));
        const auto skyLight =
            static_cast<std::uint8_t>(determineAverageValueInColumnSliceWithId(mergeSkyLights, mergeIds, id));

        if (accumulatedHeight == 0) {
            lastId            = id;
            lastBlockLight    = blockLight;
            lastSkyLight      = skyLight;
            currentMinY       = sliceMinY;
            accumulatedHeight = sliceHeight;
        } else if (id != lastId || blockLight != lastBlockLight || skyLight != lastSkyLight) {
            emit();
            lastId            = id;
            lastBlockLight    = blockLight;
            lastSkyLight      = skyLight;
            currentMinY       = sliceMinY;
            accumulatedHeight = sliceHeight;
        } else {
            accumulatedHeight += sliceHeight;
        }
    }

    if (accumulatedHeight > 0) emit();

    FullDataSource::ensureDataColumnOrder(newColumn);
    return newColumn;
}

} // namespace

bool updateFromSameDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap) {
    if (sec::detail(input.pos) != sec::detail(self.pos)) return false;

    constexpr auto DOWN  = static_cast<std::int8_t>(GenerationStep::DownSampled);
    constexpr auto EMPTY = static_cast<std::int8_t>(GenerationStep::Empty);

    bool dataChanged = false;
    for (int x = 0; x < WIDTH; x++) {
        for (int z = 0; z < WIDTH; z++) {
            const int     idx        = FullDataSource::relativePosToIndex(x, z);
            const auto    u          = static_cast<std::size_t>(idx);
            const Column& inputArray = input.columns[u];

            const std::int8_t thisGen  = self.columnGenerationStep[u];
            const std::int8_t inputGen = input.columnGenerationStep[u];

            bool allowed = false;
            if (inputGen == DOWN && (thisGen == EMPTY || thisGen == DOWN)) {
                allowed = true;
            } else if (inputGen != EMPTY && thisGen <= inputGen) {
                allowed = true;
            }
            if (!allowed) continue;

            Column& mine = self.columns[u];
            if (mine.size() != inputArray.size()) dataChanged = true;
            if (!dataChanged) {
                for (std::size_t i = 0; i < mine.size(); i++) {
                    if (mine[i] != inputArray[i]) {
                        dataChanged = true;
                        break;
                    }
                }
            }

            mine = inputArray;
            remapDataColumn(self, idx, remap);

            self.columnGenerationStep[u]       = inputGen;
            self.columnWorldCompressionMode[u] = input.columnWorldCompressionMode[u];
            self.isEmpty                       = false;
        }
    }
    return dataChanged;
}

bool updateFromOneBelowDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap) {
    if (sec::detail(input.pos) + 1 != sec::detail(self.pos)) return false;

    const sec::Pos child0        = sec::child(self.pos, 0);
    const int      recipientOffX = (sec::x(input.pos) == sec::x(child0)) ? 0 : (WIDTH / 2);
    const int      recipientOffZ = (sec::z(input.pos) == sec::z(child0)) ? 0 : (WIDTH / 2);

    bool dataChanged = false;
    for (int x = 0; x < WIDTH; x += 2) {
        for (int z = 0; z < WIDTH; z += 2) {
            const int  recipientX     = jdiv(x, 2) + recipientOffX;
            const int  recipientZ     = jdiv(z, 2) + recipientOffZ;
            const int  recipientIndex = FullDataSource::relativePosToIndex(recipientX, recipientZ);
            const auto u              = static_cast<std::size_t>(recipientIndex);

            self.columnGenerationStep[u]       = minGenStepOfTwoByTwo(input.columnGenerationStep, x, z);
            self.columnWorldCompressionMode[u] = maxCompressionOfTwoByTwo(input.columnWorldCompressionMode, x, z);

            Column merged = mergeInputTwoByTwoDataColumn(input, x, z);

            Column& mine = self.columns[u];
            if (mine.size() != merged.size()) dataChanged = true;
            if (!dataChanged) {
                for (std::size_t i = 0; i < mine.size(); i++) {
                    if (mine[i] != merged[i]) {
                        dataChanged = true;
                        break;
                    }
                }
            }

            mine = std::move(merged);
            remapDataColumn(self, recipientIndex, remap);
            self.isEmpty = false;
        }
    }
    return dataChanged;
}

bool downsampleFromOneAboveDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap) {
    if (sec::detail(input.pos) - 1 != sec::detail(self.pos)) return false;

    const sec::Pos child0    = sec::child(input.pos, 0);
    const int      inputOffX = (sec::x(self.pos) == sec::x(child0)) ? 0 : (WIDTH / 2);
    const int      inputOffZ = (sec::z(self.pos) == sec::z(child0)) ? 0 : (WIDTH / 2);

    bool dataChanged = false;
    for (int x = 0; x < WIDTH; x++) {
        for (int z = 0; z < WIDTH; z++) {
            const int  recipientIndex = FullDataSource::relativePosToIndex(x, z);
            const auto u              = static_cast<std::size_t>(recipientIndex);
            const int  inputIndex = FullDataSource::relativePosToIndex(jdiv(x, 2) + inputOffX, jdiv(z, 2) + inputOffZ);

            self.columnGenerationStep[u] = static_cast<std::int8_t>(GenerationStep::DownSampled);
            // Java quirk (:1135): the compression mode is read at recipientIndex of the INPUT, not inputIndex.
            self.columnWorldCompressionMode[u] = input.columnWorldCompressionMode[u];

            bool    downSampleColumn = true;
            Column& mine             = self.columns[u];
            for (std::uint64_t d : mine) {
                if (d != fdp::EMPTY_DATA_POINT) {
                    downSampleColumn = false;
                    break;
                }
            }

            if (downSampleColumn) {
                mine = input.columns[static_cast<std::size_t>(inputIndex)];
                remapDataColumn(self, recipientIndex, remap);
                dataChanged = true;
            }
            self.isEmpty = false;
        }
    }
    return dataChanged;
}

void removeUnusedIdsAndRemap(FullDataSource& self) {
    std::unordered_map<int, int> newIdByOldId; // lookup only
    IdMap                        newMap(self.pos);

    for (int x = 0; x < WIDTH; x++) {
        for (int z = 0; z < WIDTH; z++) {
            for (std::uint64_t dp : self.column(x, z)) {
                const int oldId     = fdp::id(dp);
                const int newId     = newMap.addIfAbsent(self.mapping.biome(oldId), self.mapping.block(oldId));
                newIdByOldId[oldId] = newId;
            }
        }
    }

    self.mapping.clear(self.pos);
    self.mapping.addAllAllowingDups(newMap);

    for (auto& col : self.columns) {
        for (std::uint64_t& dp : col) dp = fdp::setId(dp, newIdByOldId[fdp::id(dp)]);
    }
}

std::optional<bool> updateFromDataSource(FullDataSource& self, const FullDataSource& input, const UpdateOptions& opts) {
    if (input.mapping.empty()) return false;

    const int thisDetail  = sec::detail(self.pos);
    const int inputDetail = sec::detail(input.pos);

    if (inputDetail != thisDetail && inputDetail + 1 != thisDetail && inputDetail - 1 != thisDetail)
        return std::nullopt;

    const std::vector<int> remap = self.mapping.mergeAndRemap(input.mapping);

    bool dataChanged;
    if (inputDetail == thisDetail) {
        dataChanged = updateFromSameDetailLevel(self, input, remap);

        auto f = [](const std::optional<bool>& v) { return v.value_or(false); };
        if (self.applyToParent.has_value() || input.applyToParent.has_value()) {
            self.applyToParent =
                (f(self.applyToParent) || f(input.applyToParent)) && thisDetail < ROOT_SECTION_DETAIL_LEVEL;
        }
        if (self.applyToChildren.has_value() || input.applyToChildren.has_value()) {
            self.applyToChildren =
                (f(self.applyToChildren) || f(input.applyToChildren)) && thisDetail > LEAF_SECTION_DETAIL_LEVEL;
        }
        if (self.regenerateLeaf.has_value() || input.regenerateLeaf.has_value()) {
            self.regenerateLeaf = f(self.regenerateLeaf) || f(input.regenerateLeaf);
        }
    } else if (inputDetail + 1 == thisDetail) {
        dataChanged = updateFromOneBelowDetailLevel(self, input, remap);
    } else {
        dataChanged = downsampleFromOneAboveDetailLevel(self, input, remap);
    }

    // needed to prevent infinite mapped ID growth; runs regardless of dataChanged
    removeUnusedIdsAndRemap(self);

    if (dataChanged && opts.worldCompression != WorldCompressionMode::MergeSameBlocks) {
        for (int x = 0; x < WIDTH; x++)
            for (int z = 0; z < WIDTH; z++)
                if (self.column(x, z).size() > 1) cullHiddenDatapointsInColumn(self, x, z);
    }
    return dataChanged;
}

} // namespace dh
