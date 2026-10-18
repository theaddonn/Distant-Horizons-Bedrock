#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataSource.h"

/// Port of the update part of Java FullDataSourceV2 (updateFromDataSource, same-level update,
/// 2x2 child -> parent merge, parent -> child downsample, removeUnusedIdsAndRemap).
namespace dh {

/// FullDataSourceProviderV2 / FullDataUpdatePropagatorV2 constants.
constexpr int ROOT_SECTION_DETAIL_LEVEL       = 14;
constexpr int LEAF_SECTION_DETAIL_LEVEL       = 6;
constexpr int HIGHEST_DOWNSAMPLE_DETAIL_LEVEL = 12;

/// Replaces Config.Common.LodBuilding.worldCompression.
struct UpdateOptions {
    WorldCompressionMode worldCompression = WorldCompressionMode::MergeSameBlocks;
};

/// Java updateFromDataSource. nullopt = unsupported detail level combination (Java throws
/// UnsupportedOperationException); otherwise dataChanged. Returns false without touching
/// anything when input.mapping is empty. `self` is mutated; `input` is never modified.
std::optional<bool> updateFromDataSource(FullDataSource& self, const FullDataSource& input, const UpdateOptions& opts);

/// remap[oldInputId] = id in self.mapping. Return false (no change) if the detail levels don't fit
/// (Java throws IllegalArgumentException).
bool updateFromSameDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap);
bool updateFromOneBelowDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap);
bool downsampleFromOneAboveDetailLevel(FullDataSource& self, const FullDataSource& input, std::span<const int> remap);

/// Renumbers the mapping in first-use order (x outer, z inner, top -> bottom) and drops unused entries.
void removeUnusedIdsAndRemap(FullDataSource& self);

} // namespace dh
