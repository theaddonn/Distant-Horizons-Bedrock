#pragma once

// Ports of DhBlockPos and DhBlockPos2D (immutable value types). The mutable
// variant is unnecessary in C++ - just assign to a BlockPos.

#include <cmath>
#include <cstdint>
#include <string>

#include "dh/pos/Direction.h"
#include "dh/pos/LodUtil.h"
#include "dh/util/Bits.h"

namespace dh {

struct BlockPos2D;

struct BlockPos {
    std::int32_t x = 0, y = 0, z = 0;

    constexpr BlockPos() = default;
    constexpr BlockPos(std::int32_t x_, std::int32_t y_, std::int32_t z_) : x(x_), y(y_), z(z_) {}
    constexpr bool operator==(const BlockPos&) const = default;

    constexpr BlockPos createOffset(Direction d) const {
        const Int3 n = dir::normal(d);
        return createOffset(n.x, n.y, n.z);
    }
    constexpr BlockPos createOffset(std::int32_t dx, std::int32_t dy, std::int32_t dz) const {
        return {wadd(x, dx), wadd(y, dy), wadd(z, dz)};
    }

    /// x/z limited to 0..15 (y unchanged).
    constexpr BlockPos createChunkRelativePos() const {
        return {convertWorldPosToChunkRelative(x), y, convertWorldPosToChunkRelative(z)};
    }
    /// `xOrZ % 16`, then +16 if negative.
    static constexpr std::int32_t convertWorldPosToChunkRelative(std::int32_t xOrZ) {
        std::int32_t rel = jmod(xOrZ, lod::CHUNK_WIDTH);
        return rel < 0 ? wadd(rel, lod::CHUNK_WIDTH) : rel;
    }

    constexpr std::int32_t getManhattanDistance(const BlockPos& o) const {
        return wadd(wadd(jabs32(wsub(x, o.x)), jabs32(wsub(y, o.y))), jabs32(wsub(z, o.z)));
    }
    constexpr std::int32_t getManhattanDistance(std::int32_t ox, std::int32_t oz) const {
        return wadd(jabs32(wsub(x, ox)), jabs32(wsub(z, oz)));
    }

    /// Objects.hash(x, y, z) == Arrays.hashCode(new Object[]{x,y,z}).
    constexpr std::int32_t hashCode() const {
        std::int32_t r = 1;
        r              = wadd(wmul(31, r), x);
        r              = wadd(wmul(31, r), y);
        r              = wadd(wmul(31, r), z);
        return r;
    }
    /// "DHBlockPos[x, y, z]"
    std::string toString() const {
        return "DHBlockPos[" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "]";
    }
};

struct BlockPos2D {
    std::int32_t x = 0, z = 0;

    constexpr BlockPos2D() = default;
    constexpr BlockPos2D(std::int32_t x_, std::int32_t z_) : x(x_), z(z_) {}
    constexpr explicit BlockPos2D(const BlockPos& p) : x(p.x), z(p.z) {}
    constexpr bool operator==(const BlockPos2D&) const = default;

    constexpr BlockPos2D add(const BlockPos2D& o) const { return {wadd(x, o.x), wadd(z, o.z)}; }
    constexpr BlockPos2D add(std::int32_t ox, std::int32_t oz) const { return {wadd(x, ox), wadd(z, oz)}; }
    constexpr BlockPos2D subtract(const BlockPos2D& o) const { return {wsub(x, o.x), wsub(z, o.z)}; }

    /// sqrt(pow(x - ox, 2) + pow(z - oz, 2)); the int difference wraps, then squares in double.
    double dist(std::int32_t ox, std::int32_t oz) const {
        const double dx = static_cast<double>(wsub(x, ox));
        const double dz = static_cast<double>(wsub(z, oz));
        return std::sqrt(dx * dx + dz * dz);
    }
    double dist(const BlockPos2D& o) const { return dist(o.x, o.z); }

    /// pow2((long) x - ox) + pow2((long) z - oz), 64-bit wrapping.
    constexpr std::int64_t distSquared(std::int32_t ox, std::int32_t oz) const {
        const std::int64_t dx = static_cast<std::int64_t>(x) - ox;
        const std::int64_t dz = static_cast<std::int64_t>(z) - oz;
        return wadd64(wmul64(dx, dx), wmul64(dz, dz));
    }
    constexpr std::int64_t distSquared(const BlockPos2D& o) const { return distSquared(o.x, o.z); }

    constexpr std::int32_t chebyshevDist(const BlockPos2D& o) const {
        const std::int32_t a = jabs32(wsub(x, o.x));
        const std::int32_t b = jabs32(wsub(z, o.z));
        return a > b ? a : b;
    }
    constexpr std::int32_t manhattanDist(const BlockPos2D& o) const {
        return wadd(jabs32(wsub(x, o.x)), jabs32(wsub(z, o.z)));
    }

    /// Integer.hashCode(x) ^ Integer.hashCode(z)
    constexpr std::int32_t hashCode() const { return x ^ z; }
    /// "(x, z)"
    std::string toString() const { return "(" + std::to_string(x) + ", " + std::to_string(z) + ")"; }
};

inline constexpr BlockPos   BLOCK_POS_ZERO{0, 0, 0};
inline constexpr BlockPos2D BLOCK_POS2D_ZERO{0, 0};

} // namespace dh
