#pragma once

// Port of FullDataSourceProviderV2 (load/cache/save of full-data sections through ILodStore +
// the WP-D1 codec). The updater (SourceUpdater) and propagator (UpdatePropagator) hang off it.
//
// Caching (differs from Java, which re-read the DTO on every get()):
//   get() returns a SHARED, READ-ONLY snapshot. Entries live in a weak_ptr map so a section is
//   shared while anybody holds it, plus a small strong-ref LRU (ProviderOptions::strongCacheSize,
//   default 16) that keeps recently used sections alive between calls. The updater publishes the
//   section it just saved, so readers see the last committed state; any other writer of the
//   store must call invalidate(). A load racing with a publish never inserts its stale result
//   (global write epoch). Mutable work (updater/propagator) always uses loadFresh().
//
// API shape for LodQuadTree / levels (WP-G2/L):
//   get(pos)            sync; nullptr only on shutdown or unreadable/corrupt data (record erased)
//   getAsync(pos, cb)   cb runs on the file-handler executor (inline with nullptr if rejected)
//   getAdj(pos, dir)    edge strip only (decodeAdjacentOnly), not cached
//   updateAsync(src)    updater.updateAsync
//   addUpdateListener   fired (on the updating thread, pos lock held) only when data changed
//   timestamp(pos)      lastModifiedMillis, nullopt if absent

#include "dh/fulldata/FullDataSource.h"
#include "dh/fulldata/Merge.h"
#include "dh/platform/Direction.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/ILodStore.h"
#include "dh/storage/Codec.h"
#include "dh/storage/SourceUpdater.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace dh {

/// Java FullDataSourceProviderV2 constants (also in Merge.h as plain ints).
constexpr std::uint8_t kRootSectionDetailLevel = ROOT_SECTION_DETAIL_LEVEL;
constexpr std::uint8_t kLeafSectionDetailLevel = LEAF_SECTION_DETAIL_LEVEL;

struct ProviderOptions {
    UpdateOptions update;               ///< worldCompression
    int           maxSkyLight     = 15; ///< 0 for levels without sky light
    std::size_t   strongCacheSize = 16;
    IExecutor*    updateExecutor  = nullptr; ///< updateAsync target; nullptr = the file-handler executor
};

class SourceProvider {
public:
    SourceProvider(
        ILodStore&             store,
        IBlockRegistry&        registry,
        IClock&                clock,
        IExecutor&             fileHandler,
        const CodecOptions&    codec   = {},
        const ProviderOptions& options = {}
    );

    /// Loads the section, or an empty one when the store has no record (Java: createEmpty).
    std::shared_ptr<const FullDataSource> get(std::uint64_t pos);

    /// Private mutable copy straight from the store (never cached, never shared). nullptr on
    /// shutdown/error; a Corrupt record is erased first (Java get(): deleteWithKey).
    std::shared_ptr<FullDataSource> loadFresh(std::uint64_t pos);

    void getAsync(std::uint64_t pos, std::function<void(std::shared_ptr<const FullDataSource>)> cb);

    /// Only the edge strip facing `dir` (compass directions); every other column is
    /// {EMPTY_DATA_POINT}. Missing record -> createEmpty. Corrupt -> record erased + error.
    Result<std::shared_ptr<FullDataSource>> getAdj(std::uint64_t pos, Direction dir);

    std::shared_ptr<UpdateFuture> updateAsync(std::shared_ptr<const FullDataSource> src) {
        return updater_->updateAsync(std::move(src));
    }

    ListenerId addUpdateListener(UpdateListener l) { return updater_->addListener(std::move(l)); }
    void       removeUpdateListener(ListenerId id) { updater_->removeListener(id); }
    ListenerId addDeltaListener(DeltaListener l) { return updater_->addDeltaListener(std::move(l)); }
    void       removeDeltaListener(ListenerId id) { updater_->removeDeltaListener(id); }

    std::optional<std::int64_t> timestamp(std::uint64_t pos);

    /// Meta-only flag write that also drops the cached snapshot (it carries the flags).
    Status setFlag(std::uint64_t pos, StoreFlag flag, bool value);

    void flush() { (void)store_.flush(); }

    /// Drops the cache entry (call after writing the store behind the provider's back).
    void invalidate(std::uint64_t pos);
    /// Makes `src` the cached snapshot for src->pos (updater, after a successful save).
    void publish(std::shared_ptr<const FullDataSource> src);

    SourceUpdater&         updater() { return *updater_; }
    ILodStore&             store() { return store_; }
    IBlockRegistry&        registry() { return registry_; }
    IClock&                clock() { return clock_; }
    const ProviderOptions& options() const { return options_; }

    /// Marks the provider closed (further calls return nullptr/false) and clears the cache.
    /// Queued executor tasks capture `this`: callers MUST stop/drain the file-handler and update
    /// executors (and may call updater().waitQuiescent()) BEFORE destroying the provider.
    /// close() itself does not wait for queued tasks (a ManualExecutor would never run them).
    void close();
    bool isClosed() const { return shutdown_.load(); }

private:
    void touchStrongLocked(const std::shared_ptr<const FullDataSource>& s);

    ILodStore&      store_;
    IBlockRegistry& registry_;
    IClock&         clock_;
    IExecutor&      fileHandler_;
    CodecOptions    codec_;
    ProviderOptions options_;

    std::atomic<bool> shutdown_{false};

    std::mutex                                                   cacheMutex_;
    std::map<std::uint64_t, std::weak_ptr<const FullDataSource>> weak_;
    std::deque<std::shared_ptr<const FullDataSource>>            strong_; ///< back = most recent
    std::uint64_t                                                epoch_ = 0;

    std::unique_ptr<SourceUpdater> updater_;
};

} // namespace dh
