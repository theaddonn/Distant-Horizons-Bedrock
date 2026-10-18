#pragma once

// Block-face direction. Ordinal order mirrors Java EDhDirection exactly
// (DOWN, UP, NORTH, SOUTH, WEST, EAST) because the ordinal is used as a face
// index in the vertex format and the adjacent-strip arrays.
//
// WP-A owns dh/pos/ and is expected to `using Direction = dh::Direction;` there
// (or move this file) rather than define a second enum.

#include <cstdint>

namespace dh {

enum class Direction : std::uint8_t {
    Down  = 0, ///< -Y
    Up    = 1, ///< +Y
    North = 2, ///< -Z
    South = 3, ///< +Z
    West  = 4, ///< -X
    East  = 5, ///< +X
};

inline constexpr int kDirectionCount = 6;

/// Java EDhDirection.ALL iteration order: UP, DOWN, WEST, EAST, NORTH, SOUTH.
inline constexpr Direction kAllDirections[kDirectionCount] = {
    Direction::Up,
    Direction::Down,
    Direction::West,
    Direction::East,
    Direction::North,
    Direction::South,
};

/// Java EDhDirection.CARDINAL_COMPASS: EAST, WEST, SOUTH, NORTH.
inline constexpr Direction kCardinalDirections[4] = {
    Direction::East,
    Direction::West,
    Direction::South,
    Direction::North,
};

constexpr int faceIndex(Direction d) { return static_cast<int>(d); }

/// Java compassIndex: horizontal directions 0..3 (N=0, S=1, W=2, E=3), vertical -1.
constexpr int compassIndex(Direction d) {
    switch (d) {
    case Direction::North:
        return 0;
    case Direction::South:
        return 1;
    case Direction::West:
        return 2;
    case Direction::East:
        return 3;
    default:
        return -1;
    }
}

constexpr bool isVertical(Direction d) { return d == Direction::Down || d == Direction::Up; }
constexpr bool isHorizontal(Direction d) { return !isVertical(d); }

constexpr Direction opposite(Direction d) {
    switch (d) {
    case Direction::Down:
        return Direction::Up;
    case Direction::Up:
        return Direction::Down;
    case Direction::North:
        return Direction::South;
    case Direction::South:
        return Direction::North;
    case Direction::West:
        return Direction::East;
    case Direction::East:
        return Direction::West;
    }
    return d;
}

constexpr int offsetX(Direction d) { return d == Direction::East ? 1 : (d == Direction::West ? -1 : 0); }
constexpr int offsetY(Direction d) { return d == Direction::Up ? 1 : (d == Direction::Down ? -1 : 0); }
constexpr int offsetZ(Direction d) { return d == Direction::South ? 1 : (d == Direction::North ? -1 : 0); }

constexpr const char* directionName(Direction d) {
    switch (d) {
    case Direction::Down:
        return "down";
    case Direction::Up:
        return "up";
    case Direction::North:
        return "north";
    case Direction::South:
        return "south";
    case Direction::West:
        return "west";
    case Direction::East:
        return "east";
    }
    return "?";
}

} // namespace dh
