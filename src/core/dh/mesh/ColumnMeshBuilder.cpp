#include "dh/mesh/ColumnMeshBuilder.h"

#include <algorithm>

#include "dh/pos/SectionPos.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Bits.h"
#include "dh/util/Color.h"

namespace dh {

namespace {

constexpr std::uint8_t MIN_MC_LIGHT = 0;  // LodUtil.MIN_MC_LIGHT
constexpr std::uint8_t MAX_MC_LIGHT = 15; // LodUtil.MAX_MC_LIGHT

/// Java `(short) v`.
constexpr std::int16_t s16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); }

/// LodUtil.DEBUG_DETAIL_LEVEL_COLORS
const std::uint32_t DEBUG_DETAIL_LEVEL_COLORS[13] = {
    color::rgbToInt(255, 0, 0),
    color::rgbToInt(255, 127, 0),
    color::rgbToInt(255, 255, 0),
    color::rgbToInt(127, 255, 0),
    color::rgbToInt(0, 255, 0),
    color::rgbToInt(0, 255, 127),
    color::rgbToInt(0, 255, 255),
    color::rgbToInt(0, 127, 255),
    color::rgbToInt(0, 0, 255),
    color::rgbToInt(127, 0, 255),
    color::rgbToInt(255, 0, 255),
    color::rgbToInt(255, 127, 255),
    color::rgbToInt(255, 255, 255),
};

std::uint16_t tileFor(const std::uint16_t* faceTileIds, Direction d) {
    return faceTileIds == nullptr ? std::uint16_t{0} : faceTileIds[dir::faceIndex(d)];
}

/// populateColumnView never writes through the view here; the const_cast only satisfies its
/// non-const span members. A failed populate leaves the view cleared (the Java catch).
bool populateView(const ColumnRenderSource& src, ColumnRenderView& view, int x, int z) {
    if (!const_cast<ColumnRenderSource&>(src).populateColumnView(view, x, z)) {
        view.clear();
        return false;
    }
    return true;
}

/// ColumnBox.applyLightToRangeAndPopulateNewSgements
void applyLightToRange(
    const std::vector<std::uint64_t>& segments,
    std::vector<std::uint64_t>&       newSegments,
    std::int16_t                      rangeStart,
    std::int16_t                      rangeEnd,
    std::int8_t                       newLight,
    bool                              coversRange
) {
    newSegments.clear();
    for (std::uint64_t seg : segments) {
        const std::int16_t endY     = yseg::endY(seg);
        const std::int16_t startY   = yseg::startY(seg);
        const std::int8_t  skyLight = yseg::skyLight(seg);
        const bool         covered  = yseg::isCovered(seg);

        // no overlap
        if (endY <= rangeStart || startY >= rangeEnd) {
            newSegments.push_back(seg);
            continue;
        }

        // part before the range
        if (startY < rangeStart) {
            newSegments.push_back(yseg::encode(startY, rangeStart, skyLight, covered));
        }

        // overlapping part: take the minimum light
        const std::int16_t overlapStart = std::max(startY, rangeStart);
        const std::int16_t overlapEnd   = std::min(endY, rangeEnd);
        const std::int8_t  minLight     = std::min(newLight, skyLight);
        newSegments.push_back(yseg::encode(overlapStart, overlapEnd, minLight, covered || coversRange));

        // part after the range
        if (endY > rangeEnd) {
            newSegments.push_back(yseg::encode(rangeEnd, endY, skyLight, covered));
        }
    }
}

/// ColumnBox.tryAddVerticalFaceWithSkyLightToBuilder
void tryAddVerticalFace(
    LodQuadBuilder& builder,
    Direction       direction,
    std::int16_t    x,
    std::int16_t    z,
    std::int16_t    horizontalWidth,
    std::uint32_t   color,
    std::uint16_t   faceTextureId,
    std::uint8_t    irisBlockMaterialId,
    std::uint8_t    blockLight,
    std::int8_t     lastSkyLight,
    bool            covered,
    bool            inputTransparent,
    int             quadTopY,
    int             quadBottomY
) {
    // invalid positions have a negative skylight
    if (lastSkyLight < 0) {
        return;
    }
    // no transparent vertical faces unless the adjacent position is empty (ocean walls)
    if (inputTransparent && covered) {
        return;
    }
    const std::int16_t height = s16(wsub(quadTopY, quadBottomY));
    if (height <= 0) {
        return;
    }
    builder.addQuadAdj(
        direction,
        x,
        s16(quadBottomY),
        z,
        horizontalWidth,
        height,
        color,
        faceTextureId,
        irisBlockMaterialId,
        static_cast<std::uint8_t>(lastSkyLight),
        blockLight
    );
}

} // namespace

//===========//
// ColumnBox //
//===========//

namespace colbox {

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
) {
    (void)adjacentIsSameDetailLevel; // unused in Java as well

    std::vector<std::uint64_t>* segments    = &scratch.segments;
    std::vector<std::uint64_t>* newSegments = &scratch.newSegments;
    segments->clear();
    newSegments->clear();

    //============================//
    // face with no adjacent data //
    //============================//

    color = color::applyShade(color, level.shade(direction));

    if (adjColumnView.size == 0 || rdp::hasZeroHeight(adjColumnView.get(0))) {
        builder.addQuadAdj(
            direction,
            x,
            yMin,
            z,
            horizontalBlockWidth,
            ySize,
            color,
            tileFor(faceTileIds, direction),
            irisBlockMaterialId,
            MAX_MC_LIGHT,
            blockLight
        );
        return;
    }

    //=================================//
    // determine face visibility/light //
    //=================================//

    const bool         transparencyEnabled = options.quad.transparency == Transparency::Complete;
    const bool         inputTransparent    = color::getAlpha(color) < 255 && transparencyEnabled;
    const std::int16_t yMax                = s16(yMin + ySize);

    const int adjCount = adjColumnView.size;

    // start with the entire range at max light
    segments->push_back(yseg::encode(yMin, yMax, static_cast<std::int8_t>(MAX_MC_LIGHT), false));

    // index -1 is the void datapoint
    for (int adjIndex = -1; adjIndex < adjCount; adjIndex++) {
        const std::uint64_t adjPoint = (adjIndex != -1) ? adjColumnView.get(adjIndex) : adjColumnView.getVoid();
        const std::int16_t  adjMinY  = s16(rdp::yMin(adjPoint));
        const std::int16_t  adjMaxY  = s16(rdp::yMax(adjPoint));

        // skip empty adjacent points or points below this one
        if (!rdp::doesDataPointExist(adjPoint) || rdp::hasZeroHeight(adjPoint) || yMax <= adjMinY) {
            continue;
        }

        const std::uint64_t adjAbovePoint = (adjIndex > 0) ? adjColumnView.get(adjIndex - 1) : rdp::EMPTY_DATA;

        std::uint64_t adjBelowPoint;
        if (adjIndex != -1) {
            adjBelowPoint = (adjIndex + 1 < adjCount) ? adjColumnView.get(adjIndex + 1) : adjColumnView.getVoid();
        } else {
            adjBelowPoint = adjColumnView.getVoid();
        }
        // the datapoint below may be empty due to datapoint reducing: use the void datapoint
        if (adjBelowPoint == rdp::EMPTY_DATA) {
            adjBelowPoint = adjColumnView.getVoid();
        }

        const bool adjOverVoid    = !rdp::doesDataPointExist(adjBelowPoint);
        const bool adjTransparent = !adjOverVoid && rdp::alpha(adjPoint) < 255 && transparencyEnabled;

        const std::int8_t adjSkyLight = static_cast<std::int8_t>(rdp::skyLight(adjPoint));
        std::int8_t       lightToApply;

        if (!adjTransparent) {
            // adjacent is opaque. Overdraw on LOD borders prevents holes when low detail LODs
            // are replaced by higher detail ones.
            const int  borderPos = wmul(horizontalBlockWidth, ColumnRenderSource::WIDTH);
            const bool onBorder  = (direction == Direction::West && x == 0) || (direction == Direction::North && z == 0)
                               || (direction == Direction::East && x == borderPos)
                               || (direction == Direction::South && z == borderPos);

            const bool isLit = rdp::skyLight(adjPoint) != MIN_MC_LIGHT || rdp::blockLight(adjPoint) != MIN_MC_LIGHT;

            const bool useAdjLighting = onBorder && isLit && rdp::yMax(adjPoint) >= caveCullingMaxY;

            lightToApply = useAdjLighting ? adjSkyLight : SKYLIGHT_COVERED;
        } else {
            // adjacent is transparent: use the light below it
            lightToApply = static_cast<std::int8_t>(rdp::skyLight(adjBelowPoint));
        }

        // apply light to [adjMinY, adjMaxY)
        applyLightToRange(*segments, *newSegments, adjMinY, adjMaxY, lightToApply, true);
        std::swap(segments, newSegments);

        // overhang area [adjMaxY, adjAboveMinY) gets the adjacent sky light
        const std::int16_t adjAboveMinY = s16(rdp::yMin(adjAbovePoint));
        if (adjMaxY < adjAboveMinY) {
            applyLightToRange(*segments, *newSegments, adjMaxY, adjAboveMinY, adjSkyLight, false);
            std::swap(segments, newSegments);
        }
    }

    //=======================//
    // vertical faces        //
    //=======================//

    for (std::uint64_t segment : *segments) {
        tryAddVerticalFace(
            builder,
            direction,
            x,
            z,
            horizontalBlockWidth,
            color,
            tileFor(faceTileIds, direction),
            irisBlockMaterialId,
            blockLight,
            yseg::skyLight(segment),
            yseg::isCovered(segment),
            inputTransparent,
            yseg::endY(segment),
            yseg::startY(segment)
        );
    }
}

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
    const std::uint16_t*       faceTileIds
) {
    //================//
    // variable setup //
    //================//

    const std::int16_t maxX        = s16(minX + blockWidth);
    const std::int16_t maxY        = s16(minY + yHeight);
    const std::int16_t maxZ        = s16(minZ + blockWidth);
    const std::uint8_t skyLightTop = skyLight;
    const std::uint8_t skyLightBottom =
        rdp::doesDataPointExist(bottomData) ? static_cast<std::uint8_t>(rdp::skyLight(bottomData)) : MIN_MC_LIGHT;

    const bool transparencyEnabled = options.quad.transparency == Transparency::Complete;

    const bool isTransparent       = color::getAlpha(color) < 255 && transparencyEnabled;
    const bool overVoid            = !rdp::doesDataPointExist(bottomData);
    const bool isTopTransparent    = rdp::alpha(topData) < 255 && transparencyEnabled;
    const bool isBottomTransparent = rdp::alpha(bottomData) < 255 && transparencyEnabled;

    // far below anything renderable when disabled, so no extra "is enabled" check is needed
    int caveCullingMaxY = INT32_MIN;
    if (options.enableCaveCulling) {
        caveCullingMaxY = wsub(options.caveCullingHeight, level.minHeight());
    }

    // nothing below this LOD: make it opaque so the void is not visible through it
    // (still considered transparent for the checks above)
    if (!rdp::doesDataPointExist(bottomData)) {
        color = color::setAlpha(color, 255);
    }

    // never let air render, air only holds sky/block light
    if (irisBlockMaterialId == static_cast<std::uint8_t>(BlockMaterial::Air)) {
        return;
    }

    //==========================//
    // add top and bottom faces //
    //==========================//

    {
        const bool skipTop = rdp::doesDataPointExist(topData) && (rdp::yMin(topData) == maxY) && !isTopTransparent;
        if (!skipTop) {
            builder.addQuadUp(
                minX,
                maxY,
                minZ,
                blockWidth,
                color::applyShade(color, level.shade(Direction::Up)),
                tileFor(faceTileIds, Direction::Up),
                irisBlockMaterialId,
                skyLightTop,
                blockLight
            );
        }
    }
    {
        const bool skipBottom =
            rdp::doesDataPointExist(bottomData) && (rdp::yMax(bottomData) == minY) && !isBottomTransparent;
        if (!skipBottom) {
            builder.addQuadDown(
                minX,
                minY,
                minZ,
                blockWidth,
                color::applyShade(color, level.shade(Direction::Down)),
                tileFor(faceTileIds, Direction::Down),
                irisBlockMaterialId,
                skyLightBottom,
                blockLight
            );
        }
    }

    //========================================//
    // add North, south, east, and west faces //
    //========================================//

    struct Side {
        Direction    dir;
        std::int16_t x, z;
    };
    // Java order: NORTH, SOUTH, WEST, EAST
    const Side sides[4] = {
        {Direction::North, minX, minZ},
        {Direction::South, minX, maxZ},
        {Direction::West,  minX, minZ},
        {Direction::East,  maxX, minZ},
    };
    for (const Side& s : sides) {
        const std::size_t ci     = static_cast<std::size_t>(dir::compassIndex(s.dir));
        const auto*       adjCol = adjData[ci];
        if (adjCol == nullptr) {
            // the adjacent area has not been generated: opaque face, or transparent over the void
            if (!isTransparent || overVoid) {
                builder.addQuadAdj(
                    s.dir,
                    s.x,
                    minY,
                    s.z,
                    blockWidth,
                    yHeight,
                    color,
                    tileFor(faceTileIds, s.dir),
                    irisBlockMaterialId,
                    MAX_MC_LIGHT,
                    blockLight
                );
            }
        } else {
            makeAdjVerticalQuad(
                builder,
                scratch,
                level,
                options,
                *adjCol,
                faceTileIds,
                isAdjDataSameDetailLevel[ci],
                caveCullingMaxY,
                s.dir,
                s.x,
                minY,
                s.z,
                blockWidth,
                yHeight,
                color,
                irisBlockMaterialId,
                blockLight
            );
        }
    }
}

} // namespace colbox

//==============//
// vbo building //
//==============//

namespace {

/// ColumnRenderBufferBuilder.addRenderDataPointToBuilder. Returns false where Java throws.
bool addRenderDataPointToBuilder(
    const IClientLevel&        level,
    const MeshOptions&         options,
    ColumnBoxScratch&          scratch,
    std::uint64_t              renderData,
    std::uint64_t              topRenderData,
    std::uint64_t              bottomRenderData,
    const colbox::AdjViews&    adjViews,
    const std::array<bool, 4>& isSameDetailLevel,
    std::int8_t                detailLevel,
    int                        relX,
    int                        relZ,
    LodQuadBuilder&            quadBuilder
) {
    const sec::Pos     sectionPos = sec::encode(detailLevel, relX, relZ);
    const std::int16_t blockWidth = s16(sec::detailLevelWidthInBlocks(detailLevel));
    const std::int16_t blockMinX  = s16(sec::minCornerBlockX(sectionPos));
    const std::int16_t blockMinY  = s16(rdp::yMin(renderData));
    const std::int16_t blockMinZ  = s16(sec::minCornerBlockZ(sectionPos));
    const std::int16_t blockMaxY  = s16(rdp::yMax(renderData) - blockMinY);

    if (blockMaxY == 0) {
        return true;
    }
    if (blockMaxY < 0) {
        return false; // "Negative y size for the renderDataPoint!"
    }

    const std::uint8_t blockMaterialId = static_cast<std::uint8_t>(rdp::materialId(renderData));

    std::uint32_t color;
    bool          fullBright = false;
    switch (options.debugRendering) {
    case DebugRendering::Off: {
        const float saturationMultiplier = options.saturationMultiplier;
        const float brightnessMultiplier = options.brightnessMultiplier;
        if (saturationMultiplier == 1.0 && brightnessMultiplier == 1.0) {
            color = rdp::argb(renderData);
        } else {
            const auto ahsv = color::argbToAhsv(rdp::argb(renderData));
            color = color::ahsvToArgb(ahsv[0], ahsv[1], ahsv[2] * saturationMultiplier, ahsv[3] * brightnessMultiplier);
        }
        break;
    }
    case DebugRendering::ShowDetail:
        if (detailLevel < 0 || detailLevel >= 13) {
            return false; // ArrayIndexOutOfBounds in Java
        }
        color      = DEBUG_DETAIL_LEVEL_COLORS[detailLevel];
        fullBright = true;
        break;
    case DebugRendering::ShowBlockMaterial:
        switch (static_cast<BlockMaterial>(blockMaterialId)) {
        case BlockMaterial::Unknown:
        case BlockMaterial::Air:
            color = color::HOT_PINK;
            break;
        case BlockMaterial::Leaves:
            color = color::DARK_GREEN;
            break;
        case BlockMaterial::Stone:
            color = color::GRAY;
            break;
        case BlockMaterial::Wood:
            color = color::BROWN;
            break;
        case BlockMaterial::Metal:
            color = color::DARK_GRAY;
            break;
        case BlockMaterial::Dirt:
            color = color::LIGHT_BROWN;
            break;
        case BlockMaterial::Lava:
            color = color::ORANGE;
            break;
        case BlockMaterial::DeepSlate:
            color = color::BLACK;
            break;
        case BlockMaterial::Snow:
            color = color::WHITE;
            break;
        case BlockMaterial::Sand:
            color = color::TAN;
            break;
        case BlockMaterial::Terracotta:
            color = color::DARK_ORANGE;
            break;
        case BlockMaterial::NetherStone:
            color = color::DARK_RED;
            break;
        case BlockMaterial::Water:
            color = color::BLUE;
            break;
        case BlockMaterial::Grass:
            color = color::GREEN;
            break;
        case BlockMaterial::Illuminated:
            color = color::YELLOW;
            break;
        }
        fullBright = true;
        break;
    case DebugRendering::ShowOverlappingQuads:
        color      = color::WHITE;
        fullBright = true;
        break;
    default:
        return false;
    }

    colbox::addBoxQuadsToBuilder(
        quadBuilder,
        scratch,
        level,
        options,
        blockWidth,
        blockMaxY,
        blockMinX,
        blockMinY,
        blockMinZ,
        color,
        blockMaterialId,
        static_cast<std::uint8_t>(rdp::skyLight(renderData)),
        fullBright ? MAX_MC_LIGHT : static_cast<std::uint8_t>(rdp::blockLight(renderData)),
        topRenderData,
        bottomRenderData,
        adjViews,
        isSameDetailLevel,
        nullptr
    );
    return true;
}

} // namespace

bool makeLodRenderData(
    LodQuadBuilder&           quadBuilder,
    const ColumnRenderSource& self,
    const IClientLevel&       level,
    const AdjacentSources&    adj,
    const MeshOptions&        options
) {
    //=============//
    // debug check //
    //=============//

    const bool columnBuilderDebugEnabled = options.columnBuilderDebugEnable;
    if (columnBuilderDebugEnabled) {
        if (!(sec::detail(self.pos) == options.columnBuilderDebugDetailLevel
              && sec::x(self.pos) == options.columnBuilderDebugXPos
              && sec::z(self.pos) == options.columnBuilderDebugZPos)) {
            return true;
        }
    }

    //===================//
    // build each column //
    //===================//

    bool allOk = true;

    ColumnBoxScratch scratch;
    ColumnRenderView columnRenderData;
    ColumnRenderView adjStorage[4];

    colbox::AdjViews adjViews{};
    for (std::size_t i = 0; i < 4; ++i) {
        adjViews[i] = &adjStorage[i];
    }

    const std::int8_t thisDetailLevel =
        static_cast<std::int8_t>(sec::detail(self.pos) - sec::SECTION_MINIMUM_DETAIL_LEVEL);

    for (int relX = 0; relX < ColumnRenderSource::WIDTH; relX++) {
        for (int relZ = 0; relZ < ColumnRenderSource::WIDTH; relZ++) {
            if (!populateView(self, columnRenderData, relX, relZ)) {
                return false; // IllegalArgumentException escapes in Java
            }

            // ignore empty columns
            if (columnRenderData.size == 0 || !rdp::doesDataPointExist(columnRenderData.get(0))
                || rdp::hasZeroHeight(columnRenderData.get(0))) {
                continue;
            }

            if (columnBuilderDebugEnabled) {
                const int wantedX = options.columnBuilderDebugXRow;
                if (wantedX >= 0 && relX != wantedX) {
                    continue;
                }
                const int wantedZ = options.columnBuilderDebugZRow;
                if (wantedZ >= 0 && relZ != wantedZ) {
                    continue;
                }
            }

            //==================================//
            // get adjacent render data columns //
            //==================================//

            for (auto& v : adjStorage) {
                v.clear();
            }

            for (Direction direction : dir::CARDINAL_COMPASS) {
                const std::size_t ci   = static_cast<std::size_t>(dir::compassIndex(direction));
                const Int3        n    = dir::normal(direction);
                int               xAdj = relX + n.x;
                int               zAdj = relZ + n.z;
                const bool        isCrossBoundary =
                    (xAdj < 0 || xAdj >= ColumnRenderSource::WIDTH) || (zAdj < 0 || zAdj >= ColumnRenderSource::WIDTH);

                const ColumnRenderSource* adjRenderSource;
                std::int8_t               adjDetailLevel;

                if (!isCrossBoundary) {
                    adjRenderSource = &self;
                    adjDetailLevel  = thisDetailLevel;
                } else {
                    adjRenderSource = adj.bySide[ci];
                    if (adjRenderSource == nullptr) {
                        continue;
                    }
                    adjDetailLevel =
                        static_cast<std::int8_t>(sec::detail(adjRenderSource->pos) - sec::SECTION_MINIMUM_DETAIL_LEVEL);
                    if (adjDetailLevel == thisDetailLevel) {
                        // wrap the position around so it is inside the adjacent source
                        if (xAdj < 0) xAdj += ColumnRenderSource::WIDTH;
                        if (xAdj >= ColumnRenderSource::WIDTH) xAdj -= ColumnRenderSource::WIDTH;
                        if (zAdj < 0) zAdj += ColumnRenderSource::WIDTH;
                        if (zAdj >= ColumnRenderSource::WIDTH) zAdj -= ColumnRenderSource::WIDTH;
                    }
                }

                if (!((adjDetailLevel == thisDetailLevel) || (adjDetailLevel > thisDetailLevel))) {
                    // LodUtil.assertNotReach: caught by the per-direction try/catch in Java
                    // (view stays cleared = neighbour treated as missing).
                    allOk = false;
                    continue;
                }

                // A coarser neighbour is NOT wrapped, so on a section border xAdj/zAdj is -1 or 64;
                // populateColumnView throws in Java (caught, logged) and the view stays cleared.
                populateView(*adjRenderSource, adjStorage[ci], xAdj, zAdj);
            }

            //==========================//
            // build this render column //
            //==========================//

            for (int i = 0; i < columnRenderData.size; i++) {
                if (columnBuilderDebugEnabled) {
                    const int wantedColumnIndex = options.columnBuilderDebugColumnIndex;
                    if (wantedColumnIndex >= 0 && i != wantedColumnIndex) {
                        continue;
                    }
                }

                const std::uint64_t data = columnRenderData.get(i);
                // void or non-existing: nothing left in this position
                if (rdp::hasZeroHeight(data) || !rdp::doesDataPointExist(data)) {
                    break;
                }

                const std::uint64_t topDataPoint = (i - 1) >= 0 ? columnRenderData.get(i - 1) : rdp::EMPTY_DATA;
                std::uint64_t       bottomDataPoint =
                    (i + 1) < columnRenderData.size ? columnRenderData.get(i + 1) : columnRenderData.getVoid();
                // the datapoint below may be empty due to datapoint reducing: use the void datapoint
                if (bottomDataPoint == rdp::EMPTY_DATA) {
                    bottomDataPoint = columnRenderData.getVoid();
                }

                if (!addRenderDataPointToBuilder(
                        level,
                        options,
                        scratch,
                        data,
                        topDataPoint,
                        bottomDataPoint,
                        adjViews,
                        adj.sameDetail,
                        thisDetailLevel,
                        relX,
                        relZ,
                        quadBuilder
                    )) {
                    return false;
                }
            }
        }
    }

    quadBuilder.mergeQuads();
    return allOk;
}

} // namespace dh
