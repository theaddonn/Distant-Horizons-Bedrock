#pragma once

// ILodStore over an ordered key-value store (IKeyValue). See docs/STORAGE.md.
//
// Key layout (all multi-byte fields big-endian; signed coordinates are stored
// offset-binary, i.e. value ^ 0x80000000, so unsigned lexicographic key order
// equals numeric order). pos = (det, x, z):
//
//   'D' det x z        data blob                     (10 bytes)
//   'A' dir det x z    adjacent strip, dir 0..3      (11 bytes)
//   'G' det x z        genSteps blob
//   'C' det x z        compressionModes blob
//   'P' det x z        mapping ("palette") blob
//   'M' det x z        meta, 24 bytes (below)        <- the record's existence marker
//
// Order within a tag is therefore (det, x, z); tags group by kind. Meta value:
//   u8 layoutVersion(=1) u8 dataFormatVersion u8 compressionMode u8 flags
//   u32 dataChecksum i64 createdMillis i64 lastModifiedMillis
//
// Atomicity: put/erase are ONE IKeyValue::write batch. A record exists iff its
// `M` key exists (and is mirrored in the in-memory index), so even a backend
// without atomic batches would never expose a half-written record as long as
// `M` is written last. A put that creates a record also deletes every part key
// it does not supply, so orphans can never resurrect.
//
// In-memory indexes (rebuilt from the `M` prefix by load()): pos -> lastModified
// (backs exists/timestamp/timestamps/allPositions) and one set<pos> per flag
// (backs the distance queries). Ordering semantics are shared with
// MemoryLodStore through storeutil (StoreQueryUtil.h).

#include "dh/platform/ILodStore.h"
#include "dh/storage/IKeyValue.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace dh {

namespace kvkey {
std::string data(std::uint64_t pos);
std::string adjacent(int dir, std::uint64_t pos);
std::string genSteps(std::uint64_t pos);
std::string compressionModes(std::uint64_t pos);
std::string mapping(std::uint64_t pos);
std::string meta(std::uint64_t pos);
/// Inverse of meta(); nullopt if `key` is not a well-formed meta key.
std::optional<std::uint64_t>      posFromMetaKey(std::string_view key);
inline constexpr std::string_view kMetaPrefix = "M";
} // namespace kvkey

class KvLodStore : public ILodStore {
public:
    explicit KvLodStore(std::shared_ptr<IKeyValue> kv);

    /// Constructs and load()s. Fails if the key-value store cannot be iterated.
    static Result<std::unique_ptr<KvLodStore>> open(std::shared_ptr<IKeyValue> kv);

    /// Rebuilds the in-memory indexes from the `M` prefix. Corrupt meta entries
    /// are logged and treated as absent. Call once before use.
    Status load();

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
    Result<std::size_t>
    regenRecordCount(std::int32_t blockX, std::int32_t blockZ, std::int32_t maxBlockDistance) override;

    Result<std::optional<std::int64_t>>                         timestamp(std::uint64_t pos) override;
    Result<std::vector<std::pair<std::uint64_t, std::int64_t>>> timestamps(
        std::uint8_t detailLevel,
        std::int32_t startX,
        std::int32_t startZ,
        std::int32_t endX,
        std::int32_t endZ
    ) override;

    Result<std::vector<std::uint64_t>> allPositions() override;
    Status                             flush() override;

private:
    void reindexFlags(std::uint64_t pos, std::uint8_t newFlags);

    std::shared_ptr<IKeyValue>             kv_;
    std::mutex                             mutex_;
    std::map<std::uint64_t, std::int64_t>  lastModified_; // every record
    std::array<std::set<std::uint64_t>, 3> flagged_;      // == storeutil::kFlagCount
};

} // namespace dh
