#include "dh/mesh/BufferQuad.h"

#include "dh/util/Bits.h"

namespace dh {

namespace {

// Java `(long) shortValue << n`: sign-extend to 64 bits, then shift (no UB).
constexpr std::uint64_t lsh(std::int16_t v, int n) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(v)) << n;
}

constexpr std::int64_t sortKey(Direction dir, bool eastWest, std::int16_t x, std::int16_t y, std::int16_t z) {
    std::uint64_t k = 0;
    switch (dir::getAxis(dir)) {
    case Axis::X:
        k = eastWest ? (lsh(x, 48) | lsh(y, 32) | lsh(z, 16)) : (lsh(x, 48) | lsh(z, 32) | lsh(y, 16));
        break;
    case Axis::Y:
        k = eastWest ? (lsh(y, 48) | lsh(z, 32) | lsh(x, 16)) : (lsh(y, 48) | lsh(x, 32) | lsh(z, 16));
        break;
    case Axis::Z:
        k = eastWest ? (lsh(z, 48) | lsh(y, 32) | lsh(x, 16)) : (lsh(z, 48) | lsh(x, 32) | lsh(y, 16));
        break;
    }
    return static_cast<std::int64_t>(k);
}

constexpr std::int16_t wrapAdd16(std::int16_t a, std::int16_t b) {
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(a) + static_cast<std::uint16_t>(b))
    );
}

} // namespace

bool BufferQuad::set(
    std::int16_t  px,
    std::int16_t  py,
    std::int16_t  pz,
    std::int16_t  wEW,
    std::int16_t  wNS,
    std::uint32_t col,
    std::uint16_t tile,
    std::uint8_t  material,
    std::uint8_t  sky,
    std::uint8_t  block,
    Direction     dir
) {
    if (wEW <= 0 || wNS <= 0) {
        return false; // Java: "Size 0 quad!" / "Negative sized quad!"
    }
    x                       = px;
    y                       = py;
    z                       = pz;
    widthEastWest           = wEW;
    widthNorthSouthOrHeight = wNS;
    color                   = col;
    textureTileId           = tile;
    irisBlockMaterialId     = material;
    skyLight                = sky;
    blockLight              = block;
    direction               = dir;
    hasError                = false;
    sortKeyEastWest         = sortKey(dir, true, x, y, z);
    sortKeyNorthSouth       = sortKey(dir, false, x, y, z);
    return true;
}

int BufferQuad::compare(const BufferQuad& other, MergeDirection d) const {
    const std::int64_t a = d == MergeDirection::EastWest ? sortKeyEastWest : sortKeyNorthSouth;
    const std::int64_t b = d == MergeDirection::EastWest ? other.sortKeyEastWest : other.sortKeyNorthSouth;
    return a < b ? -1 : (a == b ? 0 : 1);
}

bool BufferQuad::tryMerge(BufferQuad& quad, MergeDirection mergeDirection, const QuadMergeOptions& opts) {
    if (quad.hasError || hasError) {
        return false;
    }
    if (direction != quad.direction) {
        return false;
    }
    const bool ew = mergeDirection == MergeDirection::EastWest;
    // same fixed axis
    if ((ew && y != quad.y) || (!ew && x != quad.x)) {
        return false;
    }

    int thisPerpStart, thisParStart, otherPerpStart, otherParStart;
    switch (dir::getAxis(direction)) {
    case Axis::X:
        if (ew) {
            thisPerpStart  = z;
            thisParStart   = x;
            otherPerpStart = quad.z;
            otherParStart  = quad.x;
        } else {
            thisPerpStart  = y;
            thisParStart   = z;
            otherPerpStart = quad.y;
            otherParStart  = quad.z;
        }
        break;
    case Axis::Y:
        if (ew) {
            thisPerpStart  = x;
            thisParStart   = z;
            otherPerpStart = quad.x;
            otherParStart  = quad.z;
        } else {
            thisPerpStart  = z;
            thisParStart   = y;
            otherPerpStart = quad.z;
            otherParStart  = quad.y;
        }
        break;
    case Axis::Z:
    default:
        if (ew) {
            thisPerpStart  = x;
            thisParStart   = z;
            otherPerpStart = quad.x;
            otherParStart  = quad.z;
        } else {
            // Java quirk kept as-is: parallel start is z (not y) for the NS merge on Z faces.
            thisPerpStart  = y;
            thisParStart   = z;
            otherPerpStart = quad.y;
            otherParStart  = quad.z;
        }
        break;
    }

    int thisPerpWidth, thisParWidth, otherPerpWidth, otherParWidth;
    if (ew) {
        thisPerpWidth  = widthEastWest;
        thisParWidth   = widthNorthSouthOrHeight;
        otherPerpWidth = quad.widthEastWest;
        otherParWidth  = quad.widthNorthSouthOrHeight;
    } else {
        thisPerpWidth  = widthNorthSouthOrHeight;
        thisParWidth   = widthEastWest;
        otherPerpWidth = quad.widthNorthSouthOrHeight;
        otherParWidth  = quad.widthEastWest;
    }

    const int maxQuadWidth = opts.earthCurveEnabled ? MAX_QUAD_WIDTH_FOR_EARTH_CURVATURE : NORMAL_MAX_QUAD_WIDTH;
    if (thisPerpWidth >= maxQuadWidth) { // hard width limit
        return false;
    }
    if (jfloordiv(otherPerpStart, maxQuadWidth) != jfloordiv(thisPerpStart, maxQuadWidth)) { // :277
        return false;
    }

    if (thisPerpStart + thisPerpWidth < otherPerpStart || thisParStart != otherParStart) {
        return false; // not adjacent
    } else if (thisPerpStart + thisPerpWidth > otherPerpStart) {
        if (thisPerpStart < otherPerpStart + otherPerpWidth) {
            if (opts.showOverlappingQuadErrors) {
                quad.hasError = true;
                hasError      = true;
            }
        }
        return false;
    }

    if (thisParWidth != otherParWidth) {
        return false;
    }
    if (color != quad.color || textureTileId != quad.textureTileId || irisBlockMaterialId != quad.irisBlockMaterialId
        || skyLight != quad.skyLight || blockLight != quad.blockLight) {
        return false;
    }

    if (!ew) {
        widthNorthSouthOrHeight = wrapAdd16(widthNorthSouthOrHeight, quad.widthNorthSouthOrHeight);
    } else {
        widthEastWest = wrapAdd16(widthEastWest, quad.widthEastWest);
    }
    return true;
}

} // namespace dh
