#pragma once

// In-memory ILodStore (reference implementation + test backend). Thread-safe
// via one mutex; every public call is atomic with respect to the others.
//
// Flag indexes: one ordered set of positions per StoreFlag, so the distance
// queries only visit flagged records (O(f log k), f = flagged records, k =
// result size) instead of scanning the whole store. The query point changes
// per call, so a static distance index is impossible; a bounded max-heap of
// size k gives the nearest k.

#include "dh/platform/ILodStore.h"

#include <array>
#include <map>
#include <mutex>
#include <set>

namespace dh {

class MemoryLodStore final : public ILodStore {
public:
    Status                             put(const StoreRecord& record, StorePart parts = StorePart::All) override;
    Result<std::optional<StoreRecord>> get(std::uint64_t pos, StorePart parts = StorePart::All) override;
    Result<bool>                       exists(std::uint64_t pos) override;
    Status                             erase(std::uint64_t pos) override;
    Status                             setFlag(std::uint64_t pos, StoreFlag flag, bool value) override;

    Result<std::vector<std::uint64_t>>
    positionsToPropagateUp(std::int32_t blockX, std::int32_t blockZ, std::size_t maxCount) override;
    Result<std::vector<std::uint64_t>>
    positionsToPropagateDown(std::int32_t blockX, std::int32_t blockZ, std::size_t maxCount) override;
    Result<std::vector<std::uint64_t>> positionsToRegen(
        std::int32_t blockX,
        std::int32_t blockZ,
        std::int32_t maxBlockDistance,
        std::size_t  maxCount
    ) override;
    Result<std::size_t> regenCount(std::int32_t blockX, std::int32_t blockZ, std::int32_t maxBlockDistance) override;

    Result<std::optional<std::int64_t>>                         timestamp(std::uint64_t pos) override;
    Result<std::vector<std::pair<std::uint64_t, std::int64_t>>> timestamps(
        std::uint8_t detailLevel,
        std::int32_t startX,
        std::int32_t startZ,
        std::int32_t endX,
        std::int32_t endZ
    ) override;

    Result<std::vector<std::uint64_t>> allPositions() override;
    Status                             flush() override { return Status::ok(); }

    // Test helpers.
    std::size_t size() const;
    void        clear();

private:
    static constexpr int kFlagCount = 3;
    static int           flagIndex(StoreFlag f);

    std::vector<std::uint64_t>
         nearest(int flagIdx, std::int32_t bx, std::int32_t bz, std::int64_t maxDist, std::size_t maxCount) const;
    void reindex(std::uint64_t pos, std::uint8_t oldFlags, std::uint8_t newFlags);

    mutable std::mutex                              mutex_;
    std::map<std::uint64_t, StoreRecord>            records_;
    std::array<std::set<std::uint64_t>, kFlagCount> flagged_;
};

} // namespace dh
