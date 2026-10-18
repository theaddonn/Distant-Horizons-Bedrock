#pragma once

// Port of FullDataUpdaterV2: serialised, per-position "merge input into the stored section,
// bake sky light at the leaf level, save, notify" (Java file/fullDatafile/V2/FullDataUpdaterV2.java).
//
// Locking: one KeyedLock<pos> serialises all writers of a section (Java PositionalLockProvider;
// without it two concurrent updates of one pos lose one of them, FullDataUpdaterV2:101-103).
// KeyedLock is NOT reentrant while Java's ReentrantLock was: the propagator already holds the
// position lock when it calls the updater, so it uses updateLocked() (caller holds locks()).
//
// Differences from Java: the recipient is always a private fresh copy loaded from the store
// (SourceProvider::loadFresh), never the shared cache object; after a successful save it is
// published to the provider cache (immutable from then on). A failed encode/put aborts BEFORE
// the listeners run (Java kept going with a null DTO).

#include "dh/fulldata/FullDataSource.h"
#include "dh/fulldata/Merge.h"
#include "dh/platform/IExecutor.h"
#include "dh/storage/Codec.h"
#include "dh/thread/KeyedLock.h"
#include "dh/util/Result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dh {

class SourceProvider;

using UpdateListener = std::function<void(std::uint64_t pos)>;
using ListenerId     = std::uint64_t;
/// Fired by update()/updateAsync() (NOT by the propagator) with the incoming input when it changed the
/// stored section: Java updateDataSourcesAsync(data).thenRun(...) sends only this delta to clients.
using DeltaListener = std::function<void(const std::shared_ptr<const FullDataSource>& input)>;

/// One-shot completion handle (Java CompletableFuture<Void>). First complete() wins.
class UpdateFuture {
public:
    using Callback = std::function<void(const Result<bool>&)>;

    bool isDone() const {
        std::lock_guard lock(mutex_);
        return done_;
    }
    /// Valid only when isDone(); `true` = the stored section changed.
    Result<bool> result() const {
        std::lock_guard lock(mutex_);
        if (!done_) return Result<bool>(ErrorCode::Internal, "update future not done");
        return result_;
    }
    /// Runs `cb` immediately when already done, else on completion (on the completing thread).
    void onComplete(Callback cb) {
        Result<bool> r(false);
        {
            std::lock_guard lock(mutex_);
            if (!done_) {
                callbacks_.push_back(std::move(cb));
                return;
            }
            r = result_;
        }
        cb(r);
    }
    void complete(Result<bool> r) {
        std::vector<Callback> cbs;
        {
            std::lock_guard lock(mutex_);
            if (done_) return;
            done_   = true;
            result_ = r;
            cbs.swap(callbacks_);
        }
        for (auto& cb : cbs) cb(r);
    }

private:
    mutable std::mutex    mutex_;
    bool                  done_ = false;
    Result<bool>          result_{false};
    std::vector<Callback> callbacks_;
};

struct UpdaterOptions {
    UpdateOptions update;           ///< Config.Common.LodBuilding.worldCompression
    int           maxSkyLight = 15; ///< hasSkyLight ? 15 : 0 (FullDataUpdaterV2:151)
    CodecOptions  codec;
    IExecutor*    asyncExecutor = nullptr; ///< updateAsync target; nullptr -> async calls complete as no-op
};

class SourceUpdater {
public:
    SourceUpdater(SourceProvider& provider, const UpdaterOptions& options);

    /// Java updateDataSource: locks input.pos, merges, bakes sky light iff the section detail is 6,
    /// saves, publishes, then (after releasing the lock) fires the listeners if the data changed. Returns whether it
    /// changed. Returns false (no-op) after close().
    Result<bool> update(std::shared_ptr<const FullDataSource> input);

    /// Same but the caller already holds locks().lock(input.pos) (propagator). Does NOT fire
    /// listeners: when it returns true the caller must call notifyListeners(pos) AFTER releasing
    /// the position lock (a listener may call update() for the same pos).
    Result<bool> updateLocked(const FullDataSource& input);

    /// Fires the update listeners in registration order. Never call with a position lock held.
    void notifyListeners(std::uint64_t pos);

    /// Java updateDataSourceAsync: runs update() on the async executor. A rejected/closed executor
    /// completes the future immediately with `false` (Java: completedFuture(null)).
    std::shared_ptr<UpdateFuture> updateAsync(std::shared_ptr<const FullDataSource> input);

    ListenerId addListener(UpdateListener listener);
    ListenerId addDeltaListener(DeltaListener listener);
    void       removeDeltaListener(ListenerId id);
    /// Note: an update that is already in flight may still invoke a just-removed listener (it
    /// works on a snapshot). Call waitQuiescent() before destroying the listener's target.
    void removeListener(ListenerId id);

    /// Updates currently running (inside update/updateLocked/notifyListeners).
    std::size_t inFlightUpdates() const { return inFlight_.load(); }
    /// Blocks until no update is running. Does not wait for queued-but-unstarted async updates
    /// (stop/drain the executor first) and must not be called from a listener.
    void waitQuiescent() const;

    KeyedLock<std::uint64_t>& locks() { return locks_; }

    /// Updates queued through updateAsync that have not finished (Java queuedUpdateCountsByPos).
    std::size_t pendingAsyncCount() const { return pendingAsync_.load(); }

    void close() { shutdown_.store(true); }
    bool isClosed() const { return shutdown_.load(); }

private:
    SourceProvider&          provider_;
    UpdaterOptions           options_;
    KeyedLock<std::uint64_t> locks_;

    std::atomic<bool>                shutdown_{false};
    std::atomic<std::size_t>         pendingAsync_{0};
    mutable std::atomic<std::size_t> inFlight_{0};

    std::mutex                                         listenerMutex_;
    ListenerId                                         nextListenerId_ = 1;
    std::vector<std::pair<ListenerId, DeltaListener>>  deltaListeners_;
    std::vector<std::pair<ListenerId, UpdateListener>> listeners_; ///< ascending id = registration order
};

} // namespace dh
