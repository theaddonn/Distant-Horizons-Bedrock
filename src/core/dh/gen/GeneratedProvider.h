#pragma once

// Port of GeneratedFullDataSourceProvider (file/fullDatafile/GeneratedFullDataSourceProvider.java):
// decides which sections still need world generation / retrieval and applies retrieval results.
//
// Pure decision rules (static, testable without a store):
//   requiredGenerationStep       Java :368-382
//   isFullyGenerated             Java :398-408  (column ungenerated when EMPTY, DOWN_SAMPLED or step <= required;
//                                the `<=` is deliberate: FEATURES columns do not satisfy a FEATURES requirement)
//   childNeedsGeneration         Java :433-482  (child ungenerated when min step is EMPTY, DOWN_SAMPLED or
//                                min < required; here `<` / `>=` is the Java rule, keep BOTH)
//
// Deviations from Java:
//   * IRetrievalQueue has no lowestDataDetail; it is injected (setQueueLowestDataDetail). setCanRegenerate is
//     forwarded to the queue (Java :233-241) and also mirrored in the provider (canRegenerate()).
//   * Not ported: setEstimatedRemainingRetrievalChunkCount (HUD estimate), DelayedDataSourceSaveCache.
//   * Destruction order: LodRequestModule -> retrieval queue -> provider. Queue futures hold callbacks into the
//     provider; shut the queue down (module.close()) before destroying the provider.
//   * canQueueRetrievalNow: WorldChunkUpdateManager.getTotalQueuedCount is an injected callback; the
//     DelayedDataSourceSaveCache is not ported (the provider saves synchronously through the updater).
//   * The retrieval queue is not owned (raw pointer); the caller keeps it alive until clearRetrievalQueue().
//   * onRetrievalResult uses the synchronous updater (Java updateDataSource).

#include "dh/config/Enums.h"
#include "dh/gen/RetrievalQueue.h"
#include "dh/platform/ILodStore.h"
#include "dh/pos/BlockPos.h"
#include "dh/storage/SourceProvider.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace dh {

/// Java GeneratedFullDataSourceProvider.IOnWorldGenCompleteListener.
struct IWorldGenCompleteListener {
    virtual ~IWorldGenCompleteListener()                       = default;
    virtual bool                      shouldDoWorldGen()       = 0;
    virtual std::optional<BlockPos2D> targetPosForGeneration() = 0;
    /// Fired whenever a section has completed generating (after it was stored).
    virtual void onWorldGenTaskComplete(std::uint64_t pos) = 0;
};

struct GeneratedProviderOptions {
    GeneratorPlan plan            = GeneratorPlan::SurfaceThenChunks;
    int           numberOfThreads = 2; ///< Config.Common.MultiThreading.numberOfThreads
};

class GeneratedProvider : public SourceProvider {
public:
    static constexpr int MAX_RETRIEVAL_REQUESTS_PER_THREAD = 20;

    GeneratedProvider(
        ILodStore&                      store,
        IBlockRegistry&                 registry,
        IClock&                         clock,
        IExecutor&                      fileHandler,
        const CodecOptions&             codec     = {},
        const ProviderOptions&          options   = {},
        const GeneratedProviderOptions& genOption = {}
    );

    // --- pure rules --------------------------------------------------------------
    static std::int8_t requiredGenerationStep(GeneratorPlan plan, std::int8_t sectionDetail);
    /// Java :398-408 over a non-empty step array (see header comment).
    static bool isFullyGenerated(std::span<const std::int8_t> steps, std::int8_t requiredStep);
    /// Java generationStepsAreFullyGenerated (:346): no EMPTY / DOWN_SAMPLED column.
    static bool generationStepsAreFullyGenerated(std::span<const std::int8_t> steps);
    /// Java :433-482: true when the child section must be queued. `steps` is non-empty.
    static bool childNeedsGeneration(std::span<const std::int8_t> steps, std::int8_t requiredStep);

    // --- config ---------------------------------------------------------------------
    GeneratorPlan generatorPlan() const { return static_cast<GeneratorPlan>(plan_.load()); }
    void          setGeneratorPlan(GeneratorPlan p) { plan_.store(static_cast<std::uint8_t>(p)); }
    void          setNumberOfThreads(int n) { threads_.store(n); }
    int           maxRetrievalQueueCount() const { return MAX_RETRIEVAL_REQUESTS_PER_THREAD * threads_.load(); }
    /// Java WorldChunkUpdateManager.INSTANCE.getTotalQueuedCount().
    void setQueuedChunkUpdateCounter(std::function<int()> f);

    // --- retrieval queue ----------------------------------------------------------------
    /// Java setWorldGenerationQueue: false (and no change) when a queue is already set.
    bool setRetrievalQueue(IRetrievalQueue* queue);
    void clearRetrievalQueue();
    bool hasRetrievalQueue() const;
    /// Java lowestDataDetailLevel: SECTION_MINIMUM_DETAIL_LEVEL + queue lowest detail (6 without a queue).
    std::int8_t lowestDataDetailLevel() const;
    void        setQueueLowestDataDetail(std::int8_t d) { queueLowest_.store(d); }

    bool canQueueRetrievalNow(bool pruneWaitingTasksAboveLimit = false);

    /// Java getPositionsToRetrieve. nullopt = no queue (Java null); empty = nothing to generate.
    std::optional<std::vector<std::uint64_t>> positionsToRetrieve(std::uint64_t pos, std::uint8_t generatorDetailLevel);

    /// Java queuePositionForRetrieval: nullptr without a queue. A pos already queued returns the same future.
    std::shared_ptr<RetrievalFuture> queueRetrieval(std::uint64_t pos);
    /// Java onWorldGenTaskComplete. Success -> update + listeners; Split -> nothing (the requester
    /// subdivides, see LodRequestSet); Rejected/Failed -> logged (Rejected quietly).
    void        onRetrievalResult(std::uint64_t pos, const RetrievalResult& result);
    void        removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf);
    std::size_t queuedRetrievalCount() const;

    void setCanRegenerate(bool v);
    bool canRegenerate() const { return canRegenerate_.load(); }

    void addWorldGenCompleteListener(IWorldGenCompleteListener* l);
    void removeWorldGenCompleteListener(IWorldGenCompleteListener* l);

private:
    /// nullopt = no record; empty vector = record without column data.
    std::optional<std::vector<std::int8_t>> loadSteps(std::uint64_t pos);

    std::atomic<std::uint8_t> plan_;
    std::atomic<int>          threads_;
    std::atomic<std::int8_t>  queueLowest_{0};
    std::atomic<bool>         canRegenerate_{false};

    mutable std::mutex                                        mutex_;
    IRetrievalQueue*                                          queue_ = nullptr;
    std::map<std::uint64_t, std::shared_ptr<RetrievalFuture>> queued_;
    std::function<int()>                                      queuedChunkUpdates_;

    std::mutex                              listenerMutex_;
    std::vector<IWorldGenCompleteListener*> listeners_;
};

} // namespace dh
