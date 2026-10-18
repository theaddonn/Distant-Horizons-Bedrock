#pragma once

// Port of DhChunkPos (immutable value type).

#include <cmath>
#include <cstdint>
#include <string>

#include "dh/pos/BlockPos.h"
#include "dh/pos/LodUtil.h"
#include "dh/util/Bits.h"

namespace dh {

struct ChunkPos {
    std::int32_t x = 0, z = 0;

    constexpr ChunkPos() = default;
    constexpr ChunkPos(std::int32_t x_, std::int32_t z_) : x(x_), z(z_) {}
    /// `blockPos >> 4` (floors for negatives).
    constexpr explicit ChunkPos(const BlockPos& p) : x(jshr32(p.x, 4)), z(jshr32(p.z, 4)) {}
    constexpr explicit ChunkPos(const BlockPos2D& p) : x(jshr32(p.x, 4)), z(jshr32(p.z, 4)) {}
    /// DhChunkPos(DhVec3d): `((int) pos.x) >> 4` - truncates toward zero BEFORE shifting.
    static ChunkPos fromWorldPos(double wx, double wz) {
        return {jshr32(satcast_i32(wx), 4), jshr32(satcast_i32(wz), 4)};
    }

    constexpr bool operator==(const ChunkPos&) const = default;

    /// Java: `new DhBlockPos(8 + this.x << 4, 0, 8 + this.z << 4)`; `+` binds tighter than
    /// `<<`, so this is (8 + x) << 4, i.e. x*16 + 128 - NOT the chunk centre (x*16 + 8).
    /// Preserved verbatim.
    constexpr BlockPos centerBlockPos() const { return {jshl32(wadd(8, x), 4), 0, jshl32(wadd(8, z), 4)}; }
    constexpr BlockPos minCornerBlockPos() const { return {jshl32(x, 4), 0, jshl32(z, 4)}; }

    constexpr std::int32_t getMinBlockX() const { return jshl32(x, 4); }
    constexpr std::int32_t getMinBlockZ() const { return jshl32(z, 4); }

    /// min + 16, minus 1 if that sum is negative (sic; not min + 15 for positives).
    constexpr std::int32_t getMaxBlockX() const {
        std::int32_t v = wadd(getMinBlockX(), lod::CHUNK_WIDTH);
        return v < 0 ? wadd(v, -1) : v;
    }
    constexpr std::int32_t getMaxBlockZ() const {
        std::int32_t v = wadd(getMinBlockZ(), lod::CHUNK_WIDTH);
        return v < 0 ? wadd(v, -1) : v;
    }

    constexpr BlockPos2D getMinBlockPos() const { return {jshl32(x, 4), jshl32(z, 4)}; }

    constexpr bool contains(std::int32_t bx, std::int32_t bz) const {
        const std::int32_t minX = getMinBlockX();
        const std::int32_t minZ = getMinBlockZ();
        const std::int32_t maxX = wadd(minX, lod::CHUNK_WIDTH);
        const std::int32_t maxZ = wadd(minZ, lod::CHUNK_WIDTH);
        return minX <= bx && bx < maxX && minZ <= bz && bz < maxZ;
    }
    constexpr bool contains(const BlockPos& p) const { return contains(p.x, p.z); }
    constexpr bool contains(const BlockPos2D& p) const { return contains(p.x, p.z); }

    double distance(const ChunkPos& o) const { return std::sqrt(squaredDistance(o)); }
    double squaredDistance(const ChunkPos& o) const {
        const double dx = static_cast<double>(wsub(x, o.x));
        const double dz = static_cast<double>(wsub(z, o.z));
        return dx * dx + dz * dz;
    }

    /// `x * 7309 + z` (7309 is "a random prime").
    constexpr std::int32_t hashCode() const { return wadd(wmul(x, 7309), z); }
    /// "C[x,z]"
    std::string toString() const { return "C[" + std::to_string(x) + "," + std::to_string(z) + "]"; }
};

} // namespace dh
