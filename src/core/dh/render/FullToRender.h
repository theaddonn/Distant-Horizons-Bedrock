#pragma once

// Port of Java FullDataToRenderDataTransformer (core/dataObjects/transformers).
// Converts a FullDataSource (ids + heights + light) into a ColumnRenderSource
// (colours + heights + light + material), applying block filtering, cave culling,
// the snow / water / ice special cases and vertical reduction.
//
// Not ported: applyTextureSetIds / textured LODs (TODO: re-add together with the
// texture palette of ColumnRenderSource). LodBuilderConfig is not used by the Java
// transformer, so there is nothing to port from it.

#include <optional>
#include <string_view>
#include <unordered_set>

#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/BlockInfo.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/render/ColumnRenderSource.h"
#include "dh/render/Enums.h"

namespace dh {

/// Java ObjectOpenHashSet<IBlockStateWrapper>; blocks are interned so pointer identity is state identity.
using BlockSet = std::unordered_set<const BlockInfo*>;

/// Every Config / wrapper-factory read of the Java transformer. A null set pointer means "empty set".
struct TransformOptions {
    /// Config...Quality.verticalQuality: max slices = maxVerticalData(verticalQuality, dataDetail).
    VerticalQuality verticalQuality = VerticalQuality::Medium;
    /// Config...Culling.blocksToIgnore (NON_COLLIDING enables ignoreNonCollidingBlocks).
    BlocksToAvoid blocksToAvoid = BlocksToAvoid::NonColliding;
    /// Config...Culling.tintWithAvoidedBlocks
    bool tintWithAvoidedBlocks = true;
    /// Config...Culling.enableCaveCulling (additionally off when the level hasCeiling or isEnd)
    bool enableCaveCulling = true;
    /// Config...Culling.caveCullingHeight (world Y; compared as caveCullingHeight - level.minHeight())
    int caveCullingHeight = 60;
    /// NOT read by the Java transformer; kept for the pipeline (the alpha of blockColor already carries it).
    Transparency transparency = Transparency::Complete;
    /// WRAPPER_FACTORY.getRendererIgnoredBlocks (from ignoredRenderBlockCsv); Java's ignore sets always contain air
    /// (the transformer treats air as ignored itself)
    const BlockSet* ignoredBlocks = nullptr;
    /// WRAPPER_FACTORY.getRendererIgnoredCaveBlocks (ignoredRenderCaveBlockCsv)
    const BlockSet* ignoredCaveBlocks = nullptr;
    /// WRAPPER_FACTORY.getWaterSubsurfaceReplacementBlocks (waterSubSurfaceBlockReplacementCsv)
    const BlockSet* waterSubSurfaceReplace = nullptr;
    /// WRAPPER_FACTORY.getWaterSurfaceReplacementBlocks (waterSurfaceBlockReplacementCsv)
    const BlockSet* waterSurfaceReplace = nullptr;
};

/// Java transformFullDataToRenderSource. An empty source yields an empty (not marked) render
/// source. nullopt only if a column view cannot be created (never for a valid source).
///
/// Snow layers: Java detects the 1-3 layer states of "minecraft:snow"/"minecraft:snow_layer"
/// by the "layers" property in the serial ("minecraft:snow_STATE_{layers:2}"); we accept that
/// form, "minecraft:snow[layers=2]" and the Bedrock form "minecraft:snow_layer[height=N]".
/// TODO(glue): Bedrock `height` is 0-based (height=0 is ONE layer), so layers = height + 1 and
/// only height 0..2 count as layers 1..3. Adjust if the BlockTable serial format differs.
std::optional<ColumnRenderSource> transformFullDataToRenderSource(
    const FullDataSource&   source,
    IClientLevel&           level,
    IBlockRegistry&         registry, // for water()
    const TransformOptions& options
);

/// True if `serial` is a snow layer state with 1..3 layers (exposed for tests).
bool isSnowLayerSerial(std::string_view serial);

/// Parses a comma separated block list, trims whitespace, skips empty and unresolvable names.
///
/// Every entry expands to all states of the base block (IBlockRegistry::resolveAllStates). With
/// `addAirForUnresolvable` (use for the two ignore sets, not the water-replacement sets) an entry
/// that resolves to nothing adds air, as Java getAllBlockWrappers does.
/// TODO(glue): Java derives isIce from serial.contains("minecraft:ice"); mirror that in BlockTable.
BlockSet resolveCsv(std::string_view csv, IBlockRegistry& registry, bool addAirForUnresolvable = false);

} // namespace dh
