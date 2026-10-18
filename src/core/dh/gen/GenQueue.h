#pragma once

// Port of generation/queues/WorldGenerationQueue.java (839 LOC): the world-gen
// retrieval queue. Picks waiting tasks nearest-first around a target position,
// hands them to an IWorldGenerator, bounds the number of in-flight tasks, and
// resolves each task's RetrievalFuture exactly once.
//
// Threading: all state is behind one mutex; no thread is created here. The
// queueing loop runs on `queueThread` (Java: single-thread "World Gen Queue"
// pool), generation runs on `genPool` (Java: ThreadPoolUtil.getWorldGenExecutor),
// and completion handlers run on whatever thread the generator finishes on.
// No lock is held while calling the generator, the executors or any callback.
//
// Deliberate differences from Java:
//   * nearest-task selection is deterministic (ties -> smallest encoded pos);
//     Java's ConcurrentHashMap.reduceEntries picks an arbitrary tied entry.
//   * the "queue running" flag is a real mutex-guarded state with a rerun
//     request, closing a Java liveness race where a slot freed between the loop's
//     last busy check and its `finally` was not picked up until the next event.
//   * no DhApiWorldProxy.worldLoaded()/readOnly gate (no such global here).
//   * errors that Java threw at the caller (UnsupportedOperationException,
//     assertion failures) become an already-Rejected future.
//   * the chunk-gen-time rolling average holds max(numThreads, 1) * 500 samples;
//     Java sized it Runtime.availableProcessors() * 500 (no such query here).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "dh/gen/IWorldGenerator.h"
#include "dh/gen/RetrievalQueue.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/pos/BlockPos.h"

namespace dh {

class GenQueue final : public IRetrievalQueue {
public:
    /// `numThreads` is Java Config.Common.MultiThreading.numberOfThreads (busy gate, see isGeneratorBusy()).
    GenQueue(IWorldGenerator& generator, IExecutor& queueThread, IExecutor& genPool, int numThreads, IClock& clock);
    /// Calls shutdown(true).
    ~GenQueue() override;

    GenQueue(const GenQueue&)            = delete;
    GenQueue& operator=(const GenQueue&) = delete;

    // --- IRetrievalQueue ---------------------------------------------------------
    std::shared_ptr<RetrievalFuture> submit(std::uint64_t sectionPos, std::uint8_t requiredGenDetail) override;
    void                             startAndSetTargetPos(BlockPos2D target) override;
    void                             removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) override;
    std::size_t                      waitingTaskCount() const override;
    std::size_t                      inProgressTaskCount() const override;
    void                             shutdown(bool interruptRunning) override;
    void                             setCanRegenerate(bool v) override { canRegenerate_.store(v); }
    bool                             getCanRegenerate() const override { return canRegenerate_.load(); }

    // --- Java config values read live by the queue --------------------------------
    void           setNumThreads(int numThreads);
    int            numThreads() const;
    void           setGenerationMode(GenerationMode mode); ///< Java Config.Common.WorldGenerator.chunkGeneratorMode
    GenerationMode generationMode() const;

    // --- getters (Java IFullDataSourceRetrievalQueue) ------------------------------
    std::int8_t lowestDataDetail() const;  ///< generator.largestDataDetailLevel()
    std::int8_t highestDataDetail() const; ///< generator.smallestDataDetailLevel()
    /// Java getQueuedChunkCount: sum over waiting tasks of (blockWidth / 16)^2.
    std::int32_t queuedChunkCount() const;
    /// Java getRollingAverageChunkGenTimeInMs().getAverage(); NaN until the first task completes.
    double averageChunkGenTimeMs() const;

    bool isClosing() const;
    /// Java isGeneratorBusy(): inProgress > max(numThreads, 1), or the generator says so.
    bool isGeneratorBusy() const;
    /// Java requestPosExistsWhere: true if any waiting task position satisfies `pred`.
    bool requestPosExistsWhere(const std::function<bool(std::uint64_t)>& pred) const;

    /// The ordering key (Java :302-303): Chebyshev distance from the section centre to
    /// `target`, integer-divided by the section detail level (low detail first).
    static std::int32_t taskDistance(std::uint64_t sectionPos, BlockPos2D target);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::atomic<bool>     canRegenerate_{false};
};

} // namespace dh
