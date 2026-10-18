#pragma once

// Port of core/dataObjects/render/bufferBuilding/BufferQuad.java and
// BufferMergeDirectionEnum.java.

#include <cstdint>

#include "dh/pos/Direction.h"

namespace dh {

enum class MergeDirection : std::uint8_t { EastWest, NorthSouthOrUpDown };

/// Replaces the Config reads inside BufferQuad.tryMerge.
struct QuadMergeOptions {
    /// Config...Experimental.earthCurveRatio != 0 -> max quad width is a chunk instead of 2048.
    bool earthCurveEnabled = false;
    /// Config...Debugging.showOverlappingQuadErrors.
    bool showOverlappingQuadErrors = false;
};

struct BufferQuad {
    static constexpr int NORMAL_MAX_QUAD_WIDTH              = 2048;
    static constexpr int MAX_QUAD_WIDTH_FOR_EARTH_CURVATURE = 16; // LodUtil.CHUNK_WIDTH

    std::int16_t  x = 0, y = 0, z = 0;
    std::int16_t  widthEastWest           = 0;
    std::int16_t  widthNorthSouthOrHeight = 0;
    std::uint32_t color                   = 0;
    std::uint16_t textureTileId           = 0;
    std::uint8_t  irisBlockMaterialId     = 0;
    std::uint8_t  skyLight                = 0;
    std::uint8_t  blockLight              = 0;
    Direction     direction               = Direction::Down;
    bool          hasError                = false;

    std::int64_t sortKeyEastWest   = 0;
    std::int64_t sortKeyNorthSouth = 0;

    /// Java set(): Java throws IllegalArgumentException for a zero/negative width;
    /// here the quad is left untouched and false is returned.
    bool
    set(std::int16_t  x,
        std::int16_t  y,
        std::int16_t  z,
        std::int16_t  widthEastWest,
        std::int16_t  widthNorthSouthOrHeight,
        std::uint32_t color,
        std::uint16_t textureTileId,
        std::uint8_t  irisBlockMaterialId,
        std::uint8_t  skyLight,
        std::uint8_t  blockLight,
        Direction     direction);

    /// Long.compare on the pre-computed key (-1/0/1). Directions must match (Java throws otherwise).
    int compare(const BufferQuad& other, MergeDirection d) const;

    /// Java tryMerge: true if `quad` was merged into this one.
    bool tryMerge(BufferQuad& quad, MergeDirection mergeDirection, const QuadMergeOptions& opts = {});
};

} // namespace dh
