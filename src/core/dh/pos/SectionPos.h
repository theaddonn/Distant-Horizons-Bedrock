#pragma once

// Port of core/pos/DhSectionPos.java. A section position is packed in a uint64:
//   bits  0..7   detail level (only the low 7 bits are kept: DETAIL_LEVEL_MASK = 127)
//   bits  8..35  X (28-bit two's complement)
//   bits 36..63  Z (28-bit two's complement)
//
// All int arithmetic wraps like Java's. Preconditions that make Java throw
// (IllegalArgumentException / IllegalStateException) are assert()ed; where a
// recoverable check is useful a try* variant returns std::optional.

#include <cstdint>
#include <optional>
#include <string>

#include "dh/pos/BlockPos.h"
#include "dh/pos/ChunkPos.h"
#include "dh/pos/Direction.h"
#include "dh/pos/LodUtil.h"
#include "dh/util/Bits.h"

namespace dh::sec {

using Pos = std::uint64_t;

inline constexpr std::int8_t SECTION_MINIMUM_DETAIL_LEVEL = 6;
inline constexpr std::int8_t SECTION_BLOCK_DETAIL_LEVEL   = SECTION_MINIMUM_DETAIL_LEVEL + lod::BLOCK_DETAIL_LEVEL; // 6
inline constexpr std::int8_t SECTION_CHUNK_DETAIL_LEVEL = SECTION_MINIMUM_DETAIL_LEVEL + lod::CHUNK_DETAIL_LEVEL; // 10
inline constexpr std::int8_t SECTION_REGION_DETAIL_LEVEL =
    SECTION_MINIMUM_DETAIL_LEVEL + lod::REGION_DETAIL_LEVEL; // 15

inline constexpr int DETAIL_LEVEL_WIDTH  = 8;
inline constexpr int X_POS_WIDTH         = 28;
inline constexpr int Z_POS_WIDTH         = 28;
inline constexpr int X_POS_MISSING_WIDTH = 32 - 28;
inline constexpr int Z_POS_MISSING_WIDTH = 32 - 28;

inline constexpr int DETAIL_LEVEL_OFFSET = 0;
inline constexpr int POS_X_OFFSET        = DETAIL_LEVEL_OFFSET + DETAIL_LEVEL_WIDTH; // 8
inline constexpr int POS_Z_OFFSET        = POS_X_OFFSET + X_POS_WIDTH;               // 36

inline constexpr std::uint64_t DETAIL_LEVEL_MASK = 127; // Byte.MAX_VALUE
inline constexpr std::int32_t  POS_X_MASK        = (1 << X_POS_WIDTH) - 1;
inline constexpr std::int32_t  POS_Z_MASK        = (1 << Z_POS_WIDTH) - 1;

//==============//
// constructors //
//==============//

/// No validation of the detail level or that x/z fit into 28 bits (they are masked).
constexpr Pos encode(std::int8_t detailLevel, std::int32_t x, std::int32_t z) {
    std::uint64_t data  = 0;
    data               |= static_cast<std::uint64_t>(static_cast<std::int64_t>(detailLevel)) & DETAIL_LEVEL_MASK;
    data |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & static_cast<std::uint32_t>(POS_X_MASK))
         << POS_X_OFFSET;
    data |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & static_cast<std::uint32_t>(POS_Z_MASK))
         << POS_Z_OFFSET;
    return data;
}

//==================//
// property getters //
//==================//

constexpr std::int8_t detail(Pos pos) {
    return static_cast<std::int8_t>((pos >> DETAIL_LEVEL_OFFSET) & DETAIL_LEVEL_MASK);
}
constexpr std::int32_t x(Pos pos) {
    std::int32_t v = static_cast<std::int32_t>((pos >> POS_X_OFFSET) & static_cast<std::uint64_t>(POS_X_MASK));
    return jshr32(jshl32(v, X_POS_MISSING_WIDTH), X_POS_MISSING_WIDTH); // sign-extend 28 -> 32 bits
}
constexpr std::int32_t z(Pos pos) {
    std::int32_t v = static_cast<std::int32_t>((pos >> POS_Z_OFFSET) & static_cast<std::uint64_t>(POS_Z_MASK));
    return jshr32(jshl32(v, Z_POS_MISSING_WIDTH), Z_POS_MISSING_WIDTH);
}

//==========//
// widths   //
//==========//

/// Width in blocks of one LOD at the given detail level: 1 << detail (Java masks the shift count).
constexpr std::int32_t detailLevelWidthInBlocks(std::int8_t detailLevel) { return powerOfTwo(detailLevel); }
constexpr std::int32_t blockWidth(Pos pos) { return detailLevelWidthInBlocks(detail(pos)); }
constexpr std::int32_t chunkWidth(Pos pos) { return jdiv(blockWidth(pos), lod::CHUNK_WIDTH); }

/// How many positions at returnDetailLevel span this section. Requires returnDetailLevel <= detail(pos).
constexpr std::int32_t widthCountForLowerDetailedSection(Pos pos, std::int8_t returnDetailLevel) {
    const std::int8_t d = detail(pos);
    assert(returnDetailLevel <= d);
    const std::int8_t offset = static_cast<std::int8_t>(d - returnDetailLevel);
    return powerOfTwo(offset);
}

//============//
// converters //
//============//

/// Uses the absolute section detail level. Going up floors (floorDiv), going down multiplies (wrapping int).
constexpr Pos convertToDetail(Pos pos, std::int8_t newDetailLevel) {
    const std::int8_t d  = detail(pos);
    std::int32_t      px = x(pos);
    std::int32_t      pz = z(pos);
    if (newDetailLevel >= d) {
        const std::int32_t w = powerOfTwo(newDetailLevel - d);
        px                   = jfloordiv(px, w);
        pz                   = jfloordiv(pz, w);
    } else {
        const std::int32_t w = powerOfTwo(d - newDetailLevel);
        px                   = wmul(px, w);
        pz                   = wmul(pz, w);
    }
    return encode(newDetailLevel, px, pz);
}

namespace detail_ {
/// Negative positions start at -1, so use (p + 1) / width - 1 with TRUNCATING division (not floorDiv).
constexpr std::int32_t xOrZFromChunkOrBlock(std::int32_t v, bool isChunkPos) {
    const std::int32_t width = isChunkPos ? lod::NUMB_OF_CHUNKS_WIDE : (lod::NUMB_OF_CHUNKS_WIDE * lod::CHUNK_WIDTH);
    return v < 0 ? wsub(jdiv(wadd(v, 1), width), 1) : jdiv(v, width);
}
} // namespace detail_

/// Section at outputSectionDetailLevel containing the given block column.
constexpr Pos encodeContainingBlock(std::int8_t outputSectionDetailLevel, std::int32_t blockX, std::int32_t blockZ) {
    const Pos p = encode(
        SECTION_BLOCK_DETAIL_LEVEL,
        detail_::xOrZFromChunkOrBlock(blockX, false),
        detail_::xOrZFromChunkOrBlock(blockZ, false)
    );
    return convertToDetail(p, outputSectionDetailLevel);
}
/// Section at outputSectionDetailLevel containing the given chunk.
constexpr Pos encodeContainingChunk(std::int8_t outputSectionDetailLevel, std::int32_t chunkX, std::int32_t chunkZ) {
    const Pos p = encode(
        SECTION_BLOCK_DETAIL_LEVEL,
        detail_::xOrZFromChunkOrBlock(chunkX, true),
        detail_::xOrZFromChunkOrBlock(chunkZ, true)
    );
    return convertToDetail(p, outputSectionDetailLevel);
}
constexpr Pos encodeContaining(std::int8_t outputSectionDetailLevel, const BlockPos& p) {
    return encodeContainingBlock(outputSectionDetailLevel, p.x, p.z);
}
constexpr Pos encodeContaining(std::int8_t outputSectionDetailLevel, const ChunkPos& p) {
    return encodeContainingChunk(outputSectionDetailLevel, p.x, p.z);
}

//=================//
// complex getters //
//=================//

/// Block coordinate of the section's centre. Detail 0 returns the position itself; detail 1
/// has no half-offset (so it is the min corner, not a centre).
constexpr std::int32_t centerBlockPosXOrZ(Pos pos, bool returnX) {
    const std::int8_t  d = detail(pos);
    const std::int32_t c = returnX ? x(pos) : z(pos);
    if (d == 0) {
        return c;
    }
    std::int32_t positionOffset = 0;
    if (d != 1) {
        positionOffset = powerOfTwo(d - 1);
    }
    return wadd(wmul(c, powerOfTwo(d)), positionOffset);
}
constexpr std::int32_t centerBlockX(Pos pos) { return centerBlockPosXOrZ(pos, true); }
constexpr std::int32_t centerBlockZ(Pos pos) { return centerBlockPosXOrZ(pos, false); }
constexpr BlockPos2D   centerBlockPos(Pos pos) { return {centerBlockX(pos), centerBlockZ(pos)}; }

/// Smallest block X of the section. Detail 1 is a special case (no half width subtracted).
constexpr std::int32_t minCornerBlockX(Pos pos) {
    const std::int32_t halfBlockWidth = (detail(pos) != 1) ? (blockWidth(pos) / 2) : 0;
    return wsub(centerBlockX(pos), halfBlockWidth);
}
constexpr std::int32_t minCornerBlockZ(Pos pos) {
    const std::int32_t halfBlockWidth = (detail(pos) != 1) ? (blockWidth(pos) / 2) : 0;
    return wsub(centerBlockZ(pos), halfBlockWidth);
}

constexpr std::int32_t manhattanBlockDistance(Pos pos, std::int32_t blockX, std::int32_t blockZ) {
    return wadd(jabs32(wsub(centerBlockX(pos), blockX)), jabs32(wsub(centerBlockZ(pos), blockZ)));
}
constexpr std::int32_t manhattanBlockDistance(Pos pos, const BlockPos2D& b) {
    return manhattanBlockDistance(pos, b.x, b.z);
}

/// Signed distance from a block to the section's nearest edge (negative inside).
constexpr std::int32_t chebyshevSignedDistance(Pos pos, std::int32_t blockX, std::int32_t blockZ) {
    const std::int32_t a = jabs32(wsub(centerBlockX(pos), blockX));
    const std::int32_t b = jabs32(wsub(centerBlockZ(pos), blockZ));
    return wsub(a > b ? a : b, blockWidth(pos) / 2);
}
constexpr std::int32_t chebyshevSignedDistance(Pos pos, const BlockPos& b) {
    return chebyshevSignedDistance(pos, b.x, b.z);
}
constexpr std::int32_t chebyshevSignedDistance(Pos pos, const BlockPos2D& b) {
    return chebyshevSignedDistance(pos, b.x, b.z);
}

/// getDhSectionRelativePositionForDetailLevel, verbatim (including its quirks:
/// blockOffset = max(1, 2^diff - 1) and `WIDTH-1 + (rel % 64)` for negatives).
/// Requires outputDetailLevel >= detail(pos); see tryRelativePosForDetail.
constexpr Pos relativePosForDetail(Pos pos, std::int8_t outputDetailLevel) {
    const std::int32_t xInputOriginal = x(pos);
    const std::int32_t zInputOriginal = z(pos);

    const std::int8_t detailLevelDifference = static_cast<std::int8_t>(outputDetailLevel - detail(pos));
    assert(outputDetailLevel >= detail(pos));

    std::int32_t blockOffset = wsub(powerOfTwo(detailLevelDifference), 1);
    blockOffset              = blockOffset > 1 ? blockOffset : 1; // Math.max(1, blockOffset)

    std::int32_t xInput = xInputOriginal;
    xInput              = wadd(xInput, (xInputOriginal < 0) ? blockOffset : 0);
    std::int32_t zInput = zInputOriginal;
    zInput              = wadd(zInput, (zInputOriginal < 0) ? blockOffset : 0);

    const std::int32_t w    = powerOfTwo(detailLevelDifference);
    std::int32_t       xRel = jdiv(xInput, w);
    std::int32_t       zRel = jdiv(zInput, w);

    constexpr std::int32_t WIDTH_IN = lod::FULL_DATA_WIDTH;
    constexpr std::int32_t WIDTH_EX = lod::FULL_DATA_WIDTH - 1;

    xRel = xInputOriginal >= 0 ? jmod(xRel, WIDTH_IN) : wadd(WIDTH_EX, jmod(xRel, WIDTH_IN));
    zRel = zInputOriginal >= 0 ? jmod(zRel, WIDTH_IN) : wadd(WIDTH_EX, jmod(zRel, WIDTH_IN));

    return encode(outputDetailLevel, xRel, zRel);
}
/// Like relativePosForDetail but returns nullopt where Java throws IllegalArgumentException.
constexpr std::optional<Pos> tryRelativePosForDetail(Pos pos, std::int8_t outputDetailLevel) {
    if (outputDetailLevel < detail(pos)) return std::nullopt;
    return relativePosForDetail(pos, outputDetailLevel);
}

//==================//
// parent child pos //
//==================//

/// Child at 1 detail level lower. Index -> (x, z) offset: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1),
/// i.e. x = i & 1, z = (i & 2) >> 1. Requires 0 <= child0to3 <= 3 and detail > 0.
constexpr Pos child(Pos pos, int child0to3) {
    assert(child0to3 >= 0 && child0to3 <= 3);
    assert(detail(pos) > 0);
    return encode(
        static_cast<std::int8_t>(detail(pos) - 1),
        wadd(wmul(x(pos), 2), child0to3 & 1),
        wadd(wmul(z(pos), 2), half(child0to3 & 2))
    );
}
constexpr std::optional<Pos> tryChild(Pos pos, int child0to3) {
    if (child0to3 < 0 || child0to3 > 3 || detail(pos) <= 0) return std::nullopt;
    return child(pos, child0to3);
}
/// This position's child index in its parent: (x & 1) + ((z & 1) << 1).
constexpr int childIndexOfParent(Pos pos) { return (x(pos) & 1) + square(z(pos) & 1); }

constexpr Pos parent(Pos pos) { return encode(static_cast<std::int8_t>(detail(pos) + 1), half(x(pos)), half(z(pos))); }

/// Same-detail neighbour. Java throws for UP/DOWN; use tryAdjacent for a checked variant.
constexpr Pos adjacent(Pos pos, Direction dir) {
    assert(dir != Direction::Up && dir != Direction::Down);
    const Int3 n = dir::normal(dir);
    return encode(detail(pos), wadd(x(pos), n.x), wadd(z(pos), n.z));
}
constexpr std::optional<Pos> tryAdjacent(Pos pos, Direction dir) {
    if (dir == Direction::Up || dir == Direction::Down) return std::nullopt;
    return adjacent(pos, dir);
}

//=============//
// comparisons //
//=============//

/// True if `child`'s min corner lies inside `parent` (Java contains(long, long)).
constexpr bool contains(Pos aPos, Pos bPos) {
    const std::int32_t aMinX = minCornerBlockX(aPos);
    const std::int32_t aMinZ = minCornerBlockZ(aPos);
    const std::int32_t bMinX = minCornerBlockX(bPos);
    const std::int32_t bMinZ = minCornerBlockZ(bPos);

    const std::int32_t aBlockWidth = wsub(blockWidth(aPos), 1);
    const std::int32_t aMaxX       = wadd(aMinX, aBlockWidth);
    const std::int32_t aMaxZ       = wadd(aMinZ, aBlockWidth);

    return aMinX <= bMinX && bMinX <= aMaxX && aMinZ <= bMinZ && bMinZ <= aMaxZ;
}
constexpr bool contains(Pos aPos, std::int32_t blockX, std::int32_t blockZ) {
    const std::int32_t minX = minCornerBlockX(aPos);
    const std::int32_t minZ = minCornerBlockZ(aPos);
    const std::int32_t w    = wsub(blockWidth(aPos), 1);
    const std::int32_t maxX = wadd(minX, w);
    const std::int32_t maxZ = wadd(minZ, w);
    return minX <= blockX && blockX <= maxX && minZ <= blockZ && blockZ <= maxZ;
}
constexpr bool contains(Pos aPos, const BlockPos& b) { return contains(aPos, b.x, b.z); }

//===========//
// iterators //
//===========//

/// Calls cb(Pos) for the 4 children (index order 0..3).
template <class F>
void forEachChild(Pos pos, F&& cb) {
    for (int i = 0; i < 4; i++) {
        cb(child(pos, i));
    }
}

/// Pre-order walk; cb(Pos) -> bool "stop descending here".
template <class F>
void forEachChildDownToDetailLevel(Pos pos, std::int8_t minSectionDetailLevel, F&& cb) {
    const bool stop = cb(pos);
    if (stop || minSectionDetailLevel == detail(pos)) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        forEachChildDownToDetailLevel(child(pos, i), minSectionDetailLevel, cb);
    }
}

/// Calls cb(Pos) for every descendant at exactly sectionDetailLevel.
template <class F>
void forEachChildAtDetailLevel(Pos pos, std::int8_t sectionDetailLevel, F&& cb) {
    if (sectionDetailLevel == detail(pos)) {
        cb(pos);
        return;
    }
    for (int i = 0; i < 4; i++) {
        forEachChildAtDetailLevel(child(pos, i), sectionDetailLevel, cb);
    }
}

/// Calls cb(Pos) for pos and then each ancestor up to and including maxSectionDetailLevel.
template <class F>
void forEachPosUpToDetailLevel(Pos pos, std::int8_t maxSectionDetailLevel, F&& cb) {
    cb(pos);
    if (maxSectionDetailLevel == detail(pos)) {
        return;
    }
    forEachPosUpToDetailLevel(parent(pos), maxSectionDetailLevel, cb);
}

//==============//
// base methods //
//==============//

/// Example: "6*1,-3"
std::string toString(Pos pos);
/// Long.hashCode: (int) (v ^ (v >>> 32)).
constexpr std::int32_t hashCode(Pos pos) { return static_cast<std::int32_t>(pos ^ (pos >> 32)); }

} // namespace dh::sec
