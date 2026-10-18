#pragma once

// Port of core/dataObjects/render/bufferBuilding/ColumnRenderBufferBuilder.java
// (makeLodRenderData + addRenderDataPointToBuilder; GPU upload and the object pools are
// not ported) and ColumnBox.java (+ its private YSegmentUtil).
//
// Java coordinate types are kept: block coordinates are `short` (int16_t). At data detail
// >= 9 (blockWidth >= 512) `relX * blockWidth` overflows int16 exactly as it does in Java;
// that wrap is reproduced, not fixed.

#include <array>
#include <cstdint>
#include <vector>

#include "dh/mesh/LodQuadBuilder.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/render/ColumnRenderSource.h"

namespace dh {

/// Config.Client.Advanced.Debugging.debugRenderingColors (EDhApiDebugRendering).
enum class DebugRendering : std::uint8_t { Off, ShowDetail, ShowBlockMaterial, ShowOverlappingQuads };

/// Every Config / wrapper read made by ColumnRenderBufferBuilder.java and ColumnBox.java.
struct MeshOptions {
    /// Carries transparency (Quality.transparency == COMPLETE is read by ColumnBox), grassSide, ...
    /// for the LodQuadBuilder; ColumnBox reads `quad.transparency`.
    QuadBuilderOptions quad;

    /// Graphics.Culling.enableCaveCulling (true) / caveCullingHeight (60).
    bool enableCaveCulling = true;
    int  caveCullingHeight = 60;

    /// Graphics.Quality.brightnessMultiplier / saturationMultiplier (1.0f each).
    float brightnessMultiplier = 1.0F;
    float saturationMultiplier = 1.0F;

    /// Debugging.debugRenderingColors.
    DebugRendering debugRendering = DebugRendering::Off;

    /// Debugging.ColumnBuilderDebugging.* (all off by default).
    bool columnBuilderDebugEnable      = false;
    int  columnBuilderDebugDetailLevel = 0; ///< compared with the ABSOLUTE section detail level
    int  columnBuilderDebugXPos        = 0;
    int  columnBuilderDebugZPos        = 0;
    int  columnBuilderDebugXRow        = -1; ///< < 0 = all
    int  columnBuilderDebugZRow        = -1;
    int  columnBuilderDebugColumnIndex = -1;
};

struct AdjacentSources {
    /// index = compassIndex(Direction): N, S, W, E. nullptr = no neighbour loaded.
    std::array<const ColumnRenderSource*, 4> bySide{};
    /// Carried through to makeAdjVerticalQuad, where (as in Java) it is not used.
    std::array<bool, 4> sameDetail{};
};

/// Java ColumnRenderBufferBuilder.makeLodRenderData. Fills `builder` and calls mergeQuads().
///
/// Returns false when Java would raise:
///  * an adjacent source that is FINER than `self` (LodUtil.assertNotReach). Java catches that
///    per column and carries on with the neighbour treated as missing; so does this, and the
///    whole section is still built, but false is returned at the end;
///  * `self` cannot produce a column view (empty source) or a data point has maxY < minY
///    (IllegalArgumentException escapes in Java): returns false immediately, builder partial.
bool makeLodRenderData(
    LodQuadBuilder&           builder,
    const ColumnRenderSource& self,
    const IClientLevel&       level,
    const AdjacentSources&    adj,
    const MeshOptions&        options
);

//=========//
// ColumnBox //
//=========//

/// Scratch arrays for ColumnBox.makeAdjVerticalQuad (Java: pooled LongArrayLists).
struct ColumnBoxScratch {
    std::vector<std::uint64_t> segments, newSegments;
};

/// Java ColumnBox's private YSegmentUtil: startY (16 bit) | endY (16 bit) << 16 | skyLight (8 bit) << 32 | covered
/// << 40.
namespace yseg {
constexpr std::uint64_t encode(std::int16_t startY, std::int16_t endY, std::int8_t skyLight, bool covered) {
    std::uint64_t d  = 0;
    d               |= static_cast<std::uint64_t>(static_cast<std::uint16_t>(startY));
    d               |= static_cast<std::uint64_t>(static_cast<std::uint16_t>(endY)) << 16;
    d               |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(skyLight)) << 32;
    d               |= (covered ? 1ULL : 0ULL) << 40;
    return d;
}
constexpr std::int16_t startY(std::uint64_t d) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(d)); }
constexpr std::int16_t endY(std::uint64_t d) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(d >> 16)); }
constexpr std::int8_t skyLight(std::uint64_t d) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(d >> 32)); }
constexpr bool        isCovered(std::uint64_t d) { return ((d >> 40) & 1ULL) != 0; }
} // namespace yseg

namespace colbox {

/// ColumnBox.SKYLIGHT_COVERED.
inline constexpr std::int8_t SKYLIGHT_COVERED = -1;

/// Indexed by compassIndex; nullptr is the Java `null` adjacent column (never produced by the
/// builder, which always passes a possibly-empty view).
using AdjViews = std::array<const ColumnRenderView*, 4>;

/// Java ColumnBox.addBoxQuadsToBuilder. `faceTileIds` = 6 entries indexed by face index, or nullptr (UNTEXTURED_ID =
/// 0).
void addBoxQuadsToBuilder(
    LodQuadBuilder&            builder,
    ColumnBoxScratch&          scratch,
    const IClientLevel&        level,
    const MeshOptions&         options,
    std::int16_t               blockWidth,
    std::int16_t               yHeight,
    std::int16_t               minX,
    std::int16_t               minY,
    std::int16_t               minZ,
    std::uint32_t              color,
    std::uint8_t               irisBlockMaterialId,
    std::uint8_t               skyLight,
    std::uint8_t               blockLight,
    std::uint64_t              topData,
    std::uint64_t              bottomData,
    const AdjViews&            adjData,
    const std::array<bool, 4>& isAdjDataSameDetailLevel,
    const std::uint16_t*       faceTileIds = nullptr
);

/// Java ColumnBox.makeAdjVerticalQuad (private there).
void makeAdjVerticalQuad(
    LodQuadBuilder&         builder,
    ColumnBoxScratch&       scratch,
    const IClientLevel&     level,
    const MeshOptions&      options,
    const ColumnRenderView& adjColumnView,
    const std::uint16_t*    faceTileIds,
    bool                    adjacentIsSameDetailLevel,
    int                     caveCullingMaxY,
    Direction               direction,
    std::int16_t            x,
    std::int16_t            yMin,
    std::int16_t            z,
    std::int16_t            horizontalBlockWidth,
    std::int16_t            ySize,
    std::uint32_t           color,
    std::uint8_t            irisBlockMaterialId,
    std::uint8_t            blockLight
);

} // namespace colbox
} // namespace dh
