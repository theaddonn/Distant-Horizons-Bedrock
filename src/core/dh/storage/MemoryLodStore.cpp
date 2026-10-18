#include "dh/storage/MemoryLodStore.h"

#include "dh/pos/SectionPos.h"
#include "dh/storage/StoreQueryUtil.h"

#include <algorithm>

namespace dh {

namespace {

template <class T>
void assignIfAny(StorePart parts, StorePart bit, T& dst, const T& src) {
    if (has(parts, bit)) dst = src;
}

} // namespace

int MemoryLodStore::flagIndex(StoreFlag f) { return storeutil::flagIndex(f); }

void MemoryLodStore::reindex(std::uint64_t pos, std::uint8_t oldFlags, std::uint8_t newFlags) {
    for (StoreFlag f : storeutil::kAllFlags) {
        const bool was = (oldFlags & flagBit(f)) != 0;
        const bool now = (newFlags & flagBit(f)) != 0;
        if (was == now) continue;
        auto& set = flagged_[static_cast<std::size_t>(flagIndex(f))];
        if (now) set.insert(pos);
        else set.erase(pos);
    }
}

Status MemoryLodStore::put(const StoreRecord& r, StorePart parts) {
    if (parts == StorePart::None) return Status::ok(); // nothing to write: never creates an empty record
    std::lock_guard lock(mutex_);
    auto            it = records_.find(r.pos);
    if (it == records_.end()) {
        it             = records_.emplace(r.pos, StoreRecord{}).first;
        it->second.pos = r.pos;
    }
    StoreRecord& d = it->second;
    assignIfAny(parts, StorePart::Data, d.data, r.data);
    assignIfAny(parts, StorePart::Adjacent, d.adjacent, r.adjacent);
    assignIfAny(parts, StorePart::GenSteps, d.genSteps, r.genSteps);
    assignIfAny(parts, StorePart::CompressionModes, d.compressionModes, r.compressionModes);
    assignIfAny(parts, StorePart::Mapping, d.mapping, r.mapping);
    if (has(parts, StorePart::Meta)) {
        const std::uint8_t old = d.meta.flags;
        d.meta                 = r.meta;
        reindex(r.pos, old, d.meta.flags);
    }
    return Status::ok();
}

Result<std::optional<StoreRecord>> MemoryLodStore::get(std::uint64_t pos, StorePart parts) {
    std::lock_guard lock(mutex_);
    const auto      it = records_.find(pos);
    if (it == records_.end()) return std::optional<StoreRecord>{};
    const StoreRecord& s = it->second;
    StoreRecord        out;
    out.pos = pos;
    assignIfAny(parts, StorePart::Data, out.data, s.data);
    assignIfAny(parts, StorePart::Adjacent, out.adjacent, s.adjacent);
    assignIfAny(parts, StorePart::GenSteps, out.genSteps, s.genSteps);
    assignIfAny(parts, StorePart::CompressionModes, out.compressionModes, s.compressionModes);
    assignIfAny(parts, StorePart::Mapping, out.mapping, s.mapping);
    assignIfAny(parts, StorePart::Meta, out.meta, s.meta);
    return std::optional<StoreRecord>{std::move(out)};
}

Result<bool> MemoryLodStore::exists(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    return records_.find(pos) != records_.end();
}

Status MemoryLodStore::erase(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    const auto      it = records_.find(pos);
    if (it == records_.end()) return Status::ok();
    reindex(pos, it->second.meta.flags, 0);
    records_.erase(it);
    return Status::ok();
}

Status MemoryLodStore::setFlag(std::uint64_t pos, StoreFlag flag, bool value) {
    std::lock_guard lock(mutex_);
    const auto      it = records_.find(pos);
    if (it == records_.end()) return Status(ErrorCode::NotFound, "setFlag: no record at pos");
    StoreMeta&         m   = it->second.meta;
    const std::uint8_t old = m.flags;
    m.setFlag(flag, value);
    reindex(pos, old, m.flags);
    return Status::ok();
}

std::vector<std::uint64_t> MemoryLodStore::nearest(
    int          flagIdx,
    std::int32_t bx,
    std::int32_t bz,
    std::int64_t maxDist,
    std::size_t  maxCount
) const {
    return storeutil::nearest(flagged_[static_cast<std::size_t>(flagIdx)], bx, bz, maxDist, maxCount);
}

Result<std::vector<std::uint64_t>>
MemoryLodStore::positionsToPropagateUp(std::int32_t bx, std::int32_t bz, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return nearest(flagIndex(StoreFlag::ApplyToParent), bx, bz, -1, maxCount);
}

Result<std::vector<std::uint64_t>>
MemoryLodStore::positionsToPropagateDown(std::int32_t bx, std::int32_t bz, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return nearest(flagIndex(StoreFlag::ApplyToChildren), bx, bz, -1, maxCount);
}

Result<std::vector<std::uint64_t>> MemoryLodStore::positionsToRegen(
    std::int32_t bx,
    std::int32_t bz,
    std::int32_t maxBlockDistance,
    std::size_t  maxCount
) {
    std::lock_guard lock(mutex_);
    return nearest(flagIndex(StoreFlag::Regenerate), bx, bz, maxBlockDistance, maxCount); // <0 = everything (Java)
}

Result<std::size_t> MemoryLodStore::regenRecordCount(std::int32_t bx, std::int32_t bz, std::int32_t maxBlockDistance) {
    std::lock_guard lock(mutex_);
    const auto&     set = flagged_[static_cast<std::size_t>(flagIndex(StoreFlag::Regenerate))];
    return storeutil::countWithin(set, bx, bz, maxBlockDistance);
}

Result<std::optional<std::int64_t>> MemoryLodStore::timestamp(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    const auto      it = records_.find(pos);
    if (it == records_.end()) return std::optional<std::int64_t>{};
    return std::optional<std::int64_t>{it->second.meta.lastModifiedMillis};
}

Result<std::vector<std::pair<std::uint64_t, std::int64_t>>> MemoryLodStore::timestamps(
    std::uint8_t detailLevel,
    std::int32_t startX,
    std::int32_t startZ,
    std::int32_t endX,
    std::int32_t endZ
) {
    std::lock_guard                                     lock(mutex_);
    std::vector<std::pair<std::uint64_t, std::int64_t>> out;
    for (const auto& [pos, rec] : records_) {
        if (!storeutil::inTimestampRange(pos, detailLevel, startX, startZ, endX, endZ)) continue;
        out.emplace_back(pos, rec.meta.lastModifiedMillis);
    }
    return out;
}

Result<std::vector<std::uint64_t>> MemoryLodStore::allPositions() {
    std::lock_guard            lock(mutex_);
    std::vector<std::uint64_t> out;
    out.reserve(records_.size());
    for (const auto& kv : records_) out.push_back(kv.first);
    return out;
}

std::size_t MemoryLodStore::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

void MemoryLodStore::clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
    for (auto& s : flagged_) s.clear();
}

} // namespace dh
