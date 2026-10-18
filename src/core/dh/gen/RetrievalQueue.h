#pragma once

// Port of generation/queues/IFullDataSourceRetrievalQueue.java plus the
// generation/tasks/{DataSourceRetrievalResult,ERetrievalResultState}.java value
// types and the CompletableFuture<DataSourceRetrievalResult> the Java queue hands
// out (RetrievalFuture).
//
// A retrieval queue tracks the full data sources the system wants but does not
// have yet (positions to world-generate, or to download from a server) and
// resolves them nearest-first around a moving target position.
//
// Java had two implementations: WorldGenerationQueue (-> gen/GenQueue.h) and
// RemoteWorldRetrievalQueue (WP-K2). Both share this interface.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dh/pos/BlockPos.h"

namespace dh {

struct FullDataSource;

/// Java DataSourceRetrievalResult + ERetrievalResultState, plus the two ways a
/// Java future could complete exceptionally (CancellationException -> Rejected,
/// anything else -> Failed).
struct RetrievalResult {
    enum class Kind : std::uint8_t {
        Success,  ///< `source` holds the generated data (Java SUCCESS)
        Split,    ///< the generator cannot produce this detail level; the caller must subdivide
                  ///< (Java REQUIRES_SPLITTING, dataSource == null, pos == 0)
        Rejected, ///< never attempted: cancelled, removed, or the queue was shutting down
                  ///< (Java CancellationException / UnsupportedOperationException)
        Failed,   ///< the generator reported an error (Java completeExceptionally(e))
    };

    Kind                            kind = Kind::Rejected;
    std::uint64_t                   pos  = 0; ///< generated section pos; 0 unless Success (as Java)
    std::shared_ptr<FullDataSource> source;
    std::string                     error; ///< human-readable reason for Rejected / Failed

    static RetrievalResult success(std::uint64_t pos, std::shared_ptr<FullDataSource> source) {
        return RetrievalResult{Kind::Success, pos, std::move(source), {}};
    }
    static RetrievalResult split() { return RetrievalResult{Kind::Split, 0, nullptr, {}}; }
    static RetrievalResult rejected(std::string reason) {
        return RetrievalResult{Kind::Rejected, 0, nullptr, std::move(reason)};
    }
    static RetrievalResult failed(std::string reason) {
        return RetrievalResult{Kind::Failed, 0, nullptr, std::move(reason)};
    }
};

/// Thread-safe one-shot future (Java CompletableFuture<DataSourceRetrievalResult>).
///
///   * The first complete()/cancel() wins; later calls are ignored (CompletableFuture semantics).
///   * onComplete() callbacks run exactly once each: on the completing thread, inside
///     complete()/cancel(); or immediately on the caller's thread if already done.
///     No lock is held while callbacks run, so a callback may call back into the future.
///   * cancel() completes the future with Kind::Rejected and sets isCancelled().
class RetrievalFuture {
public:
    using Callback = std::function<void(const RetrievalResult&)>;

    RetrievalFuture()                                  = default;
    RetrievalFuture(const RetrievalFuture&)            = delete;
    RetrievalFuture& operator=(const RetrievalFuture&) = delete;

    bool isDone() const {
        std::lock_guard lock(mutex_);
        return result_.has_value();
    }
    bool isCancelled() const {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }
    /// nullopt until done.
    std::optional<RetrievalResult> get() const {
        std::lock_guard lock(mutex_);
        return result_;
    }

    void onComplete(Callback callback) {
        if (!callback) return;
        {
            std::lock_guard lock(mutex_);
            if (!result_.has_value()) {
                callbacks_.push_back(std::move(callback));
                return;
            }
        }
        // already done: run inline (Java runs the dependent stage on the caller's thread)
        const std::optional<RetrievalResult> snapshot = get();
        callback(*snapshot);
    }

    /// Owner-side completion. Returns true if this call completed the future.
    bool complete(RetrievalResult result) { return completeImpl(std::move(result), false); }

    /// Java cancel(): completes with Rejected("cancelled"). Returns true if this call won.
    bool cancel() { return completeImpl(RetrievalResult::rejected("cancelled"), true); }

private:
    bool completeImpl(RetrievalResult result, bool asCancel) {
        std::vector<Callback> toRun;
        {
            std::lock_guard lock(mutex_);
            if (result_.has_value()) return false;
            result_    = std::move(result);
            cancelled_ = asCancel;
            toRun.swap(callbacks_);
        }
        // Copy the result so callbacks see a stable value without holding the lock.
        const std::optional<RetrievalResult> snapshot = get();
        for (Callback& cb : toRun) cb(*snapshot);
        return true;
    }

    mutable std::mutex             mutex_;
    std::optional<RetrievalResult> result_;
    bool                           cancelled_ = false;
    std::vector<Callback>          callbacks_;
};

/// Java IFullDataSourceRetrievalQueue (the parts the core needs; the F3/progress
/// display getters live with the HUD code).
struct IRetrievalQueue {
    virtual ~IRetrievalQueue() = default;

    /// Java submitRetrievalTask(pos, requiredDataDetail). `requiredGenDetail` is an absolute
    /// data detail level (0 = block). A pos already waiting returns the same future. Never null.
    virtual std::shared_ptr<RetrievalFuture> submit(std::uint64_t sectionPos, std::uint8_t requiredGenDetail) = 0;

    /// Starts the retrieval loop if not running and (re)centres the nearest-first order on `target`.
    virtual void startAndSetTargetPos(BlockPos2D target) = 0;

    /// Cancels every waiting (not yet started) request for which `removeIf(pos)` returns true.
    virtual void removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) = 0;

    /// Java get/setCanRegenerate: gate for regeneration tasks (set by the requester each tick: true once no
    /// block-detail section is missing). Default: ignored. GenQueue stores it; no regeneration scheduling reads
    /// it in this port yet (Java WorldGenerationQueue only stores it too).
    virtual void setCanRegenerate(bool) {}
    virtual bool getCanRegenerate() const { return false; }

    virtual std::size_t waitingTaskCount() const    = 0;
    virtual std::size_t inProgressTaskCount() const = 0;

    /// Java startClosingAsync(true, interruptRunning) + close(): no new requests, waiting
    /// requests are cancelled, in-progress futures are cancelled. With interruptRunning the
    /// generator is closed at once; otherwise it is closed when the last running task ends.
    virtual void shutdown(bool interruptRunning) = 0;
};

} // namespace dh
