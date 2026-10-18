#pragma once

// Unchanged-chunk skip (Java: sql/repo/ChunkHashRepo + AbstractDhLevel.getChunkHash /
// onDataSourceSaveAsync). A chunk's content hash is persisted only AFTER the section that
// contains it has been written, so a crash can never leave a hash without its data.
//
// Intended flow for WP-L (AbstractDhLevel.updateChunkAsync):
//   1. caller: if (hashes.get(chunkPos) == newHash) skip;            // getChunkHash, 0 = unknown
//   2. src = createFromChunk(...); tracker.notePending(src->pos, chunkPos, newHash);
//   3. merge into the delayed save cache (mutate inside the first lock, see PORTING.md)
//   4. when the cache hands over the merged section:
//        provider.updateAsync(section)->onComplete([&](const Result<bool>& r) {
//            if (r.has_value() && !provider.isClosed()) tracker.commit(section->pos);
//            else tracker.discard(section->pos);
//        });
//      Outcomes of the update future:
//        Ok(true)   section changed and was saved            -> commit
//        Ok(false)  section unchanged (already up to date)   -> commit, but ONLY if the provider is
//                   not closed: updateAsync also completes with Ok(false) when it was rejected or
//                   the updater was shut down, which must never persist a hash
//        Error      load/encode/put failed                   -> discard
//      Never commit on rejection / shutdown / error: the hash would then claim data that was
//      not written and the chunk would be skipped forever.
// The platform glue later provides a persistent IChunkHashStore (KV table); MemoryChunkHashStore
// is the reference implementation and test backend.

#include "dh/pos/ChunkPos.h"
#include "dh/util/Result.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace dh {

struct ChunkHashEntry {
    ChunkPos     pos;
    std::int32_t hash = 0;
};

struct IChunkHashStore {
    virtual ~IChunkHashStore() = default;

    /// Stored hash, 0 when unknown (Java getChunkHash). Thread-safe.
    virtual std::int32_t get(ChunkPos pos) = 0;
    /// Upserts all entries in one batch (Java: one transaction).
    virtual Status putBatch(std::span<const ChunkHashEntry> entries) = 0;
};

class MemoryChunkHashStore final : public IChunkHashStore {
public:
    std::int32_t get(ChunkPos pos) override {
        std::lock_guard lock(mutex_);
        auto            it = map_.find({pos.x, pos.z});
        return it == map_.end() ? 0 : it->second;
    }
    Status putBatch(std::span<const ChunkHashEntry> entries) override {
        std::lock_guard lock(mutex_);
        for (const ChunkHashEntry& e : entries) map_[{e.pos.x, e.pos.z}] = e.hash;
        return Status::ok();
    }
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return map_.size();
    }

private:
    mutable std::mutex                                            mutex_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> map_;
};

/// Java updatedChunkPosSetBySectionPos + updatedChunkHashesByChunkPos. Remembers which chunks
/// went into a section until that section is durably saved, then writes their hashes.
/// Ordered containers: commit order is (chunk x, chunk z) ascending, never hash order.
class ChunkHashTracker {
public:
    explicit ChunkHashTracker(IChunkHashStore& store) : store_(store) {}

    void notePending(std::uint64_t sectionPos, ChunkPos chunk, std::int32_t hash) {
        std::lock_guard lock(mutex_);
        bySection_[sectionPos][{chunk.x, chunk.z}] = hash;
    }

    /// Persists and forgets the pending hashes of `sectionPos`; returns how many were written.
    /// Call after the section update finished successfully.
    std::size_t commit(std::uint64_t sectionPos) {
        std::vector<ChunkHashEntry> entries;
        {
            std::lock_guard lock(mutex_);
            auto            it = bySection_.find(sectionPos);
            if (it == bySection_.end()) return 0;
            for (const auto& kv : it->second)
                entries.push_back({
                    {kv.first.first, kv.first.second},
                    kv.second
                });
            bySection_.erase(it);
        }
        if (entries.empty()) return 0;
        if (!store_.putBatch(entries)) return 0;
        return entries.size();
    }

    /// Drops pending hashes without saving (failed update).
    void discard(std::uint64_t sectionPos) {
        std::lock_guard lock(mutex_);
        bySection_.erase(sectionPos);
    }

    std::size_t pendingSectionCount() const {
        std::lock_guard lock(mutex_);
        return bySection_.size();
    }

private:
    using ChunkMap = std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t>;

    IChunkHashStore&                  store_;
    mutable std::mutex                mutex_;
    std::map<std::uint64_t, ChunkMap> bySection_;
};

} // namespace dh
