#pragma once

// Ordering/filter helpers shared by every ILodStore implementation so that the
// distance semantics cannot drift between MemoryLodStore and KvLodStore.

#include "dh/platform/ILodStore.h"
#include "dh/pos/SectionPos.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace dh::storeutil {

inline constexpr int kFlagCount = 3;

inline int flagIndex(StoreFlag f) {
    switch (f) {
    case StoreFlag::ApplyToParent:
        return 0;
    case StoreFlag::ApplyToChildren:
        return 1;
    case StoreFlag::Regenerate:
    default:
        return 2;
    }
}

inline constexpr StoreFlag kAllFlags[kFlagCount] =
    {StoreFlag::ApplyToParent, StoreFlag::ApplyToChildren, StoreFlag::Regenerate};

/// Java: abs((PosX << (6+DetailLevel)) - ?) + abs(...) i.e. Manhattan distance
/// to the section's min corner, evaluated in 64 bit like SQLite (int32 wrap of
/// sec::minCornerBlockX/Z would differ).
inline std::int64_t distanceToMinCorner(std::uint64_t pos, std::int32_t bx, std::int32_t bz) {
    const std::int64_t cx = static_cast<std::int64_t>(sec::x(pos)) * (std::int64_t{1} << sec::detail(pos));
    const std::int64_t cz = static_cast<std::int64_t>(sec::z(pos)) * (std::int64_t{1} << sec::detail(pos));
    const std::int64_t dx = cx - bx;
    const std::int64_t dz = cz - bz;
    return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
}

/// Nearest `maxCount` of `flagged` by (distance, pos); maxDist < 0 = unlimited.
inline std::vector<std::uint64_t> nearest(
    const std::set<std::uint64_t>& flagged,
    std::int32_t                   bx,
    std::int32_t                   bz,
    std::int64_t                   maxDist,
    std::size_t                    maxCount
) {
    using Entry = std::pair<std::int64_t, std::uint64_t>;
    if (maxCount == 0) return {};
    std::priority_queue<Entry> heap; // max-heap, worst candidate on top
    for (std::uint64_t pos : flagged) {
        const std::int64_t dist = distanceToMinCorner(pos, bx, bz);
        if (maxDist >= 0 && dist > maxDist) continue;
        const Entry e{dist, pos};
        if (heap.size() < maxCount) {
            heap.push(e);
        } else if (e < heap.top()) {
            heap.pop();
            heap.push(e);
        }
    }
    std::vector<std::uint64_t> out(heap.size());
    for (std::size_t i = out.size(); i-- > 0;) {
        out[i] = heap.top().second;
        heap.pop();
    }
    return out;
}

/// Number of `flagged` positions within maxDist (inclusive); maxDist < 0 = all.
inline std::size_t
countWithin(const std::set<std::uint64_t>& flagged, std::int32_t bx, std::int32_t bz, std::int64_t maxDist) {
    if (maxDist < 0) return flagged.size();
    std::size_t n = 0;
    for (std::uint64_t pos : flagged) {
        if (distanceToMinCorner(pos, bx, bz) <= maxDist) ++n;
    }
    return n;
}

/// Section x/z in [startX,endX) x [startZ,endZ) at `detailLevel` (end exclusive).
inline bool inTimestampRange(
    std::uint64_t pos,
    std::uint8_t  detailLevel,
    std::int32_t  startX,
    std::int32_t  startZ,
    std::int32_t  endX,
    std::int32_t  endZ
) {
    if (static_cast<std::uint8_t>(sec::detail(pos)) != detailLevel) return false;
    const std::int32_t x = sec::x(pos);
    const std::int32_t z = sec::z(pos);
    return !(x < startX || x >= endX || z < startZ || z >= endZ);
}

} // namespace dh::storeutil
