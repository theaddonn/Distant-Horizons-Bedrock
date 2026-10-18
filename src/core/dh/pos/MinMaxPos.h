#pragma once

// Port of sql/dto/util/FullDataMinMaxPosUtil.java: four 16-bit values packed in a long.

#include <cstdint>
#include <optional>

#include "dh/pos/Direction.h"
#include "dh/pos/LodUtil.h"

namespace dh::minmax {

inline constexpr std::int32_t ADJ_POS_MASK = 0xFFFF;
inline constexpr int          MIN_X_OFFSET = 0;
inline constexpr int          MAX_X_OFFSET = 16;
inline constexpr int          MIN_Z_OFFSET = 32;
inline constexpr int          MAX_Z_OFFSET = 48;

/// Java: each short is widened to long first (sign-extended) and OR-ed in, so a
/// negative value smears into the higher fields. Preserved.
constexpr std::int64_t encodeAdjMinMaxPos(std::int16_t minX, std::int16_t maxX, std::int16_t minZ, std::int16_t maxZ) {
    std::uint64_t data  = 0;
    data               |= static_cast<std::uint64_t>(static_cast<std::int64_t>(minX)) << MIN_X_OFFSET;
    data               |= static_cast<std::uint64_t>(static_cast<std::int64_t>(maxX)) << MAX_X_OFFSET;
    data               |= static_cast<std::uint64_t>(static_cast<std::int64_t>(minZ)) << MIN_Z_OFFSET;
    data               |= static_cast<std::uint64_t>(static_cast<std::int64_t>(maxZ)) << MAX_Z_OFFSET;
    return static_cast<std::int64_t>(data);
}

constexpr std::int32_t getAdjMinX(std::int64_t e) {
    return static_cast<std::int32_t>((static_cast<std::uint64_t>(e) >> MIN_X_OFFSET) & ADJ_POS_MASK);
}
constexpr std::int32_t getAdjMaxX(std::int64_t e) {
    return static_cast<std::int32_t>((static_cast<std::uint64_t>(e) >> MAX_X_OFFSET) & ADJ_POS_MASK);
}
constexpr std::int32_t getAdjMinZ(std::int64_t e) {
    return static_cast<std::int32_t>((static_cast<std::uint64_t>(e) >> MIN_Z_OFFSET) & ADJ_POS_MASK);
}
constexpr std::int32_t getAdjMaxZ(std::int64_t e) {
    return static_cast<std::int32_t>((static_cast<std::uint64_t>(e) >> MAX_Z_OFFSET) & ADJ_POS_MASK);
}

/// getEncodedMinMaxPos(direction): the one-wide edge strip of a 64x64 data source.
/// Java throws IllegalArgumentException for UP/DOWN; here that is nullopt.
constexpr std::optional<std::int64_t> getEncodedMinMaxPos(Direction d) {
    constexpr std::int16_t W = lod::FULL_DATA_WIDTH;
    switch (d) {
    case Direction::North:
        return encodeAdjMinMaxPos(0, W, 0, 1);
    case Direction::South:
        return encodeAdjMinMaxPos(0, W, W - 1, W);
    case Direction::East:
        return encodeAdjMinMaxPos(W - 1, W, 0, W);
    case Direction::West:
        return encodeAdjMinMaxPos(0, 1, 0, W);
    default:
        return std::nullopt;
    }
}

} // namespace dh::minmax
