#pragma once

// Port of generation/queues/LodRequestModule.java plus the request bookkeeping LodQuadTree keeps around it
// (LodQuadTree.java ~:880-1040: missingGenerationPosSet / queuedGenerationPosSet, startQueuedRetrievalTasks,
// tryQueuePosForRetrieval).
//
//   LodRequestModule  start/stop of the retrieval queue as the listener's shouldDoWorldGen() flips and the
//                     periodic "retarget the queue" step. Java ran a 20 ms ticker thread (after a 500 ms
//                     wait); here tick() is exactly one iteration and the caller owns the timer.
//   LodRequestSet     the de-duplicating missing/queued position sets + nearest-first queuing loop.
//
// Destruction order: LodRequestModule (stops/shuts down the queue) -> retrieval queue -> provider; a
// LodRequestSet must outlive the queue (pending futures call back into it).
//
// Deviations: the module owns the queue through a factory (Java AbstractLodRequestState supplier) and shuts
// it down with IRetrievalQueue::shutdown(true); the read-only gate (DhApiWorldProxy.tryGetReadOnly) is an
// injected predicate; the progress-message thread of AbstractLodRequestState is not ported.
// Missing positions are kept in a std::set (ascending pos) so the nearest-first sort has a deterministic
// tie-break (Java sorted a HashSet copy with a stable sort).

#include "dh/gen/GeneratedProvider.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace dh {

class LodRequestModule {
public:
    using QueueFactory = std::function<std::unique_ptr<IRetrievalQueue>()>;

    LodRequestModule(GeneratedProvider& provider, IWorldGenCompleteListener& listener, QueueFactory factory);
    ~LodRequestModule();

    LodRequestModule(const LodRequestModule&)            = delete;
    LodRequestModule& operator=(const LodRequestModule&) = delete;

    /// DhApiWorldProxy.tryGetReadOnly: true = never generate.
    void setReadOnlyPredicate(std::function<bool()> p) { readOnly_ = std::move(p); }

    /// One iteration of Java tickLoop/tick.
    void tick();

    void startWorldGen();
    void stopWorldGen();
    bool isWorldGenRunning() const { return queue_ != nullptr; }
    /// The running queue (nullptr when stopped); for debug displays.
    IRetrievalQueue* queue() const { return queue_.get(); }

    /// Java close(): stops world gen (queue shutdown) and detaches from the provider.
    void close();

private:
    GeneratedProvider&               provider_;
    IWorldGenCompleteListener&       listener_;
    QueueFactory                     factory_;
    std::function<bool()>            readOnly_;
    std::unique_ptr<IRetrievalQueue> queue_;
};

/// De-duplicated bookkeeping of which positions are missing / queued.
class LodRequestSet {
public:
    /// Java tryQueuePosForRetrieval: adds the provider's positionsToRetrieve(pos, detail(pos)) that are not
    /// already queued to the missing set. No-op when the provider has no queue.
    void tryQueuePosForRetrieval(GeneratedProvider& provider, std::uint64_t pos);

    void addMissing(std::uint64_t pos);
    bool isMissing(std::uint64_t pos) const;
    bool isQueued(std::uint64_t pos) const;
    /// Forget everything.
    void clear();
    /// Java LodQuadTree.onConfigValueSet: clear() and setCanRegenerate(false) (the requester then re-adds
    /// every section's missing positions).
    void onConfigChanged(GeneratedProvider& provider);

    std::size_t missingCount() const;
    std::size_t queuedCount() const;

    /// Missing positions nearest first (Manhattan block distance to `target`, ties by ascending pos).
    std::vector<std::uint64_t> sortedMissing(BlockPos2D target) const;

    /// Java startQueuedRetrievalTasks: walks sortedMissing; stops when !provider.canQueueRetrievalNow();
    /// skips positions rejected by `posInRange`; queues the rest. A finished task leaves the queued set;
    /// Split queues the 4 children; Rejected/Failed put the position back into the missing set.
    /// Afterwards sets the regenerate gate like LodQuadTree :938-985: missing list empty -> true; otherwise
    /// true only if some missing position is at the block detail level (6) ("no low-detail LOD still missing").
    /// Returns the number of newly queued positions.
    std::size_t startQueued(
        GeneratedProvider&                        provider,
        BlockPos2D                                target,
        const std::function<bool(std::uint64_t)>& posInRange = {}
    );

private:
    void onFinished(GeneratedProvider& provider, std::uint64_t pos, const RetrievalResult& r);

    mutable std::mutex      mutex_;
    std::set<std::uint64_t> missing_;
    std::set<std::uint64_t> queued_;
};

} // namespace dh
