#include "dh/storage/KvLodStore.h"

#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/StoreQueryUtil.h"

#include <algorithm>

namespace dh {

namespace kvkey {

namespace {

void putU32(std::string& s, std::uint32_t v) {
    for (int i = 3; i >= 0; i--) s.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}
void putI32(std::string& s, std::int32_t v) { putU32(s, static_cast<std::uint32_t>(v) ^ 0x80000000u); }
void putU64(std::string& s, std::uint64_t v) {
    for (int i = 7; i >= 0; i--) s.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

std::uint32_t getU32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    return v;
}
std::uint64_t getU64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    return v;
}

void appendPos(std::string& s, std::uint64_t pos) {
    s.push_back(static_cast<char>(static_cast<std::uint8_t>(sec::detail(pos))));
    putI32(s, sec::x(pos));
    putI32(s, sec::z(pos));
}

std::string simple(char tag, std::uint64_t pos) {
    std::string k;
    k.reserve(10);
    k.push_back(tag);
    appendPos(k, pos);
    return k;
}

} // namespace

std::string data(std::uint64_t pos) { return simple('D', pos); }
std::string genSteps(std::uint64_t pos) { return simple('G', pos); }
std::string compressionModes(std::uint64_t pos) { return simple('C', pos); }
std::string mapping(std::uint64_t pos) { return simple('P', pos); }
std::string meta(std::uint64_t pos) { return simple('M', pos); }
std::string adjacent(int dir, std::uint64_t pos) {
    std::string k;
    k.reserve(11);
    k.push_back('A');
    k.push_back(static_cast<char>(dir));
    appendPos(k, pos);
    return k;
}

std::optional<std::uint64_t> posFromMetaKey(std::string_view key) {
    if (key.size() != 10 || key[0] != 'M') return std::nullopt;
    const std::int8_t  det = static_cast<std::int8_t>(static_cast<std::uint8_t>(key[1]));
    const std::int32_t x   = static_cast<std::int32_t>(getU32(key.data() + 2) ^ 0x80000000u);
    const std::int32_t z   = static_cast<std::int32_t>(getU32(key.data() + 6) ^ 0x80000000u);
    return sec::encode(det, x, z);
}

// Used by the meta codec below.
std::uint32_t readU32(const char* p) { return getU32(p); }
std::uint64_t readU64(const char* p) { return getU64(p); }
void          writeU32(std::string& s, std::uint32_t v) { putU32(s, v); }
void          writeU64(std::string& s, std::uint64_t v) { putU64(s, v); }

} // namespace kvkey

namespace {

constexpr std::size_t kMetaSize     = 24;
constexpr char        kMetaVersion  = 1;
constexpr int         kAdjacentDirs = 4;

std::string encodeMeta(const StoreMeta& m) {
    std::string s;
    s.reserve(kMetaSize);
    s.push_back(kMetaVersion);
    s.push_back(static_cast<char>(m.dataFormatVersion));
    s.push_back(static_cast<char>(m.compressionMode));
    s.push_back(static_cast<char>(m.flags));
    kvkey::writeU32(s, m.dataChecksum);
    kvkey::writeU64(s, static_cast<std::uint64_t>(m.createdMillis));
    kvkey::writeU64(s, static_cast<std::uint64_t>(m.lastModifiedMillis));
    return s;
}

std::optional<StoreMeta> decodeMeta(std::string_view v) {
    if (v.size() != kMetaSize || v[0] != kMetaVersion) return std::nullopt;
    StoreMeta m;
    m.dataFormatVersion  = static_cast<std::uint8_t>(v[1]);
    m.compressionMode    = static_cast<std::uint8_t>(v[2]);
    m.flags              = static_cast<std::uint8_t>(v[3]);
    m.dataChecksum       = kvkey::readU32(v.data() + 4);
    m.createdMillis      = static_cast<std::int64_t>(kvkey::readU64(v.data() + 8));
    m.lastModifiedMillis = static_cast<std::int64_t>(kvkey::readU64(v.data() + 16));
    return m;
}

std::string toString(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::vector<std::byte> toBytes(const std::optional<std::string>& s) {
    std::vector<std::byte> out;
    if (!s) return out;
    out.resize(s->size());
    if (!s->empty()) std::copy_n(reinterpret_cast<const std::byte*>(s->data()), s->size(), out.begin());
    return out;
}

} // namespace

KvLodStore::KvLodStore(std::shared_ptr<IKeyValue> kv) : kv_(std::move(kv)) {}

Result<std::unique_ptr<KvLodStore>> KvLodStore::open(std::shared_ptr<IKeyValue> kv) {
    if (!kv) return Error(ErrorCode::InvalidArgument, "KvLodStore::open: null key-value store");
    auto store = std::make_unique<KvLodStore>(std::move(kv));
    auto st    = store->load();
    if (!st) return st.error();
    return store;
}

Status KvLodStore::load() {
    std::lock_guard lock(mutex_);
    lastModified_.clear();
    for (auto& s : flagged_) s.clear();
    std::size_t skipped = 0;
    auto        st      = kv_->iteratePrefix(kvkey::kMetaPrefix, [&](std::string_view k, std::string_view v) {
        const auto pos = kvkey::posFromMetaKey(k);
        const auto m   = decodeMeta(v);
        if (!pos || !m) {
            ++skipped;
            return true;
        }
        lastModified_[*pos] = m->lastModifiedMillis;
        for (StoreFlag f : storeutil::kAllFlags) {
            if (m->hasFlag(f)) flagged_[static_cast<std::size_t>(storeutil::flagIndex(f))].insert(*pos);
        }
        return true;
    });
    if (!st) return st;
    if (skipped > 0) log::logf(LogLevel::Warn, "KvLodStore: skipped %zu corrupt meta entries at open", skipped);
    return Status::ok();
}


void KvLodStore::reindexFlags(std::uint64_t pos, std::uint8_t newFlags) {
    for (StoreFlag f : storeutil::kAllFlags) {
        auto& set = flagged_[static_cast<std::size_t>(storeutil::flagIndex(f))];
        if ((newFlags & flagBit(f)) != 0) set.insert(pos);
        else set.erase(pos);
    }
}

Status KvLodStore::put(const StoreRecord& r, StorePart parts) {
    if (parts == StorePart::None) return Status::ok();
    std::lock_guard lock(mutex_);
    const bool      creating = lastModified_.find(r.pos) == lastModified_.end();
    KvBatch         batch;
    // When creating, parts not supplied are deleted so stale orphans cannot resurface.
    auto emit = [&](StorePart bit, std::string key, const std::vector<std::byte>& blob) {
        if (has(parts, bit)) batch.set(std::move(key), toString(blob));
        else if (creating) batch.del(std::move(key));
    };
    emit(StorePart::Data, kvkey::data(r.pos), r.data);
    if (has(parts, StorePart::Adjacent)) {
        for (int i = 0; i < kAdjacentDirs; i++)
            batch.set(kvkey::adjacent(i, r.pos), toString(r.adjacent[static_cast<std::size_t>(i)]));
    } else if (creating) {
        for (int i = 0; i < kAdjacentDirs; i++) batch.del(kvkey::adjacent(i, r.pos));
    }
    emit(StorePart::GenSteps, kvkey::genSteps(r.pos), r.genSteps);
    emit(StorePart::CompressionModes, kvkey::compressionModes(r.pos), r.compressionModes);
    emit(StorePart::Mapping, kvkey::mapping(r.pos), r.mapping);

    StoreMeta newMeta; // defaults when a partial put creates the record (MemoryLodStore parity)
    bool      writeMeta = false;
    if (has(parts, StorePart::Meta)) {
        newMeta   = r.meta;
        writeMeta = true;
    } else if (creating) {
        writeMeta = true;
    }
    if (writeMeta) batch.set(kvkey::meta(r.pos), encodeMeta(newMeta)); // last in the batch

    auto st = kv_->write(batch);
    if (!st) return st;
    if (writeMeta) {
        lastModified_[r.pos] = newMeta.lastModifiedMillis;
        reindexFlags(r.pos, newMeta.flags);
    }
    return Status::ok();
}

Result<std::optional<StoreRecord>> KvLodStore::get(std::uint64_t pos, StorePart parts) {
    std::lock_guard lock(mutex_);
    if (lastModified_.find(pos) == lastModified_.end()) return std::optional<StoreRecord>{};
    StoreRecord out;
    out.pos = pos;

    auto read = [&](StorePart bit, const std::string& key, std::vector<std::byte>& dst) -> Status {
        if (!has(parts, bit)) return Status::ok();
        auto v = kv_->get(key);
        if (!v) return v.error();
        dst = toBytes(*v);
        return Status::ok();
    };
    if (auto s = read(StorePart::Data, kvkey::data(pos), out.data); !s) return s.error();
    if (has(parts, StorePart::Adjacent)) {
        for (int i = 0; i < kAdjacentDirs; i++) {
            auto v = kv_->get(kvkey::adjacent(i, pos));
            if (!v) return v.error();
            out.adjacent[static_cast<std::size_t>(i)] = toBytes(*v);
        }
    }
    if (auto s = read(StorePart::GenSteps, kvkey::genSteps(pos), out.genSteps); !s) return s.error();
    if (auto s = read(StorePart::CompressionModes, kvkey::compressionModes(pos), out.compressionModes); !s)
        return s.error();
    if (auto s = read(StorePart::Mapping, kvkey::mapping(pos), out.mapping); !s) return s.error();
    if (has(parts, StorePart::Meta)) {
        auto v = kv_->get(kvkey::meta(pos));
        if (!v) return v.error();
        if (!v->has_value()) return Error(ErrorCode::Corrupt, "KvLodStore: indexed record has no meta key");
        auto m = decodeMeta(**v);
        if (!m) return Error(ErrorCode::Corrupt, "KvLodStore: malformed meta value");
        out.meta = *m;
    }
    return std::optional<StoreRecord>{std::move(out)};
}

Result<bool> KvLodStore::exists(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    return lastModified_.find(pos) != lastModified_.end();
}

Status KvLodStore::erase(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    if (lastModified_.find(pos) == lastModified_.end()) return Status::ok();
    KvBatch batch;
    batch.del(kvkey::data(pos));
    for (int i = 0; i < kAdjacentDirs; i++) batch.del(kvkey::adjacent(i, pos));
    batch.del(kvkey::genSteps(pos));
    batch.del(kvkey::compressionModes(pos));
    batch.del(kvkey::mapping(pos));
    batch.del(kvkey::meta(pos));
    auto st = kv_->write(batch);
    if (!st) return st;
    lastModified_.erase(pos);
    reindexFlags(pos, 0);
    return Status::ok();
}

Status KvLodStore::setFlag(std::uint64_t pos, StoreFlag flag, bool value) {
    std::lock_guard lock(mutex_);
    if (lastModified_.find(pos) == lastModified_.end()) return Status(ErrorCode::NotFound, "setFlag: no record at pos");
    auto v = kv_->get(kvkey::meta(pos));
    if (!v) return v.error();
    if (!v->has_value()) return Status(ErrorCode::Corrupt, "KvLodStore: indexed record has no meta key");
    auto m = decodeMeta(**v);
    if (!m) return Status(ErrorCode::Corrupt, "KvLodStore: malformed meta value");
    m->setFlag(flag, value);
    KvBatch batch;
    batch.set(kvkey::meta(pos), encodeMeta(*m));
    auto st = kv_->write(batch);
    if (!st) return st;
    reindexFlags(pos, m->flags);
    return Status::ok();
}

Result<std::vector<std::uint64_t>>
KvLodStore::positionsToPropagateUp(std::int32_t bx, std::int32_t bz, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return storeutil::nearest(
        flagged_[static_cast<std::size_t>(storeutil::flagIndex(StoreFlag::ApplyToParent))],
        bx,
        bz,
        -1,
        maxCount
    );
}

Result<std::vector<std::uint64_t>>
KvLodStore::positionsToPropagateDown(std::int32_t bx, std::int32_t bz, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return storeutil::nearest(
        flagged_[static_cast<std::size_t>(storeutil::flagIndex(StoreFlag::ApplyToChildren))],
        bx,
        bz,
        -1,
        maxCount
    );
}

Result<std::vector<std::uint64_t>>
KvLodStore::positionsToRegen(std::int32_t bx, std::int32_t bz, std::int32_t maxBlockDistance, std::size_t maxCount) {
    std::lock_guard lock(mutex_);
    return storeutil::nearest(
        flagged_[static_cast<std::size_t>(storeutil::flagIndex(StoreFlag::Regenerate))],
        bx,
        bz,
        maxBlockDistance,
        maxCount
    );
}

Result<std::size_t> KvLodStore::regenRecordCount(std::int32_t bx, std::int32_t bz, std::int32_t maxBlockDistance) {
    std::lock_guard lock(mutex_);
    return storeutil::countWithin(
        flagged_[static_cast<std::size_t>(storeutil::flagIndex(StoreFlag::Regenerate))],
        bx,
        bz,
        maxBlockDistance
    );
}

Result<std::optional<std::int64_t>> KvLodStore::timestamp(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    const auto      it = lastModified_.find(pos);
    if (it == lastModified_.end()) return std::optional<std::int64_t>{};
    return std::optional<std::int64_t>{it->second};
}

Result<std::vector<std::pair<std::uint64_t, std::int64_t>>> KvLodStore::timestamps(
    std::uint8_t detailLevel,
    std::int32_t startX,
    std::int32_t startZ,
    std::int32_t endX,
    std::int32_t endZ
) {
    std::lock_guard                                     lock(mutex_);
    std::vector<std::pair<std::uint64_t, std::int64_t>> out;
    for (const auto& [pos, ts] : lastModified_) {
        if (!storeutil::inTimestampRange(pos, detailLevel, startX, startZ, endX, endZ)) continue;
        out.emplace_back(pos, ts);
    }
    return out;
}

Result<std::vector<std::uint64_t>> KvLodStore::allPositions() {
    std::lock_guard            lock(mutex_);
    std::vector<std::uint64_t> out;
    out.reserve(lastModified_.size());
    for (const auto& kv : lastModified_) out.push_back(kv.first);
    return out;
}

Status KvLodStore::flush() {
    std::lock_guard lock(mutex_);
    return kv_->flush();
}

} // namespace dh
