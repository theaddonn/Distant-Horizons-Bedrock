#pragma once

// Port of core/enums/EDhDirection.java helpers. The enum itself lives in
// dh/platform/Direction.h (shared with the platform contracts); this header
// adds the axis/normal/name tables and the Java-ordered direction arrays.

#include "dh/platform/Direction.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace dh {

enum class Axis : std::uint8_t { X, Y, Z };
enum class AxisDirection : std::uint8_t { Positive, Negative };

struct Int3 {
    std::int32_t   x, y, z;
    constexpr bool operator==(const Int3&) const = default;
};

namespace dir {

namespace detail {
struct Info {
    std::string_view name;
    AxisDirection    axisDirection;
    Axis             axis;
    Int3             normal;
    int              compassIndex; // -1 when not a compass direction
    int              faceIndex;
};
inline constexpr Info INFO[6] = {
    {"down",  AxisDirection::Negative, Axis::Y, {0, -1, 0}, -1, 0},
    {"up",    AxisDirection::Positive, Axis::Y, {0, 1, 0},  -1, 1},
    {"north", AxisDirection::Negative, Axis::Z, {0, 0, -1}, 0,  2},
    {"south", AxisDirection::Positive, Axis::Z, {0, 0, 1},  1,  3},
    {"west",  AxisDirection::Negative, Axis::X, {-1, 0, 0}, 2,  4},
    {"east",  AxisDirection::Positive, Axis::X, {1, 0, 0},  3,  5},
};
} // namespace detail

constexpr const detail::Info& info(Direction d) { return detail::INFO[static_cast<int>(d)]; }

constexpr Direction opposite(Direction d) {
    switch (d) {
    case Direction::Up:
        return Direction::Down;
    case Direction::Down:
        return Direction::Up;
    case Direction::North:
        return Direction::South;
    case Direction::South:
        return Direction::North;
    case Direction::East:
        return Direction::West;
    case Direction::West:
        return Direction::East;
    }
    return d;
}
constexpr Int3             normal(Direction d) { return info(d).normal; }
constexpr Axis             getAxis(Direction d) { return info(d).axis; }
constexpr AxisDirection    getAxisDirection(Direction d) { return info(d).axisDirection; }
constexpr int              compassIndex(Direction d) { return info(d).compassIndex; }
constexpr int              faceIndex(Direction d) { return info(d).faceIndex; }
constexpr std::string_view name(Direction d) { return info(d).name; }

/// EDhDirection.ALL: Up, Down, West, East, North, South.
inline constexpr std::array<Direction, 6> ALL =
    {Direction::Up, Direction::Down, Direction::West, Direction::East, Direction::North, Direction::South};
/// EDhDirection.CARDINAL_COMPASS (in this order): East, West, South, North.
inline constexpr std::array<Direction, 4> CARDINAL_COMPASS =
    {Direction::East, Direction::West, Direction::South, Direction::North};
/// Horizontal neighbours; same set and order as CARDINAL_COMPASS.
inline constexpr std::array<Direction, 4> ADJ_DIRECTIONS = CARDINAL_COMPASS;

} // namespace dir
} // namespace dh
