#pragma once

// dh::IChunkGenerator for the BDS server DLL: produces chunks that are NOT loaded in the world.
//
// Strategy (spike, nothing here could be run offline - see the internal server-driver spike notes):
//   Surface / Features:
//     1. scratch chunk: the dimension's WorldGenerator IS a ChunkSource (`class WorldGenerator : public
//        ChunkSource`), so `generator->createNewChunk(cp, LoadMode::None, false)` returns a fresh, empty,
//        unloaded `shared_ptr<LevelChunk>` that is not registered with any chunk source. If that returns
//        null (base ChunkSource without a level), fall back to the exported `LevelChunk` constructor.
//     2. `ChunkState` Unloaded -> Generating via `LevelChunk::tryChangeState`.
//     3. `generator->loadChunk(*lc, false)`: the virtual that vanilla's getOrLoadChunk calls to fill a new
//        chunk (OverworldGenerator::$loadChunk -> prepareHeights, biomes, surface builder, carvers). This is
//        "base terrain" = Surface mode. No structures / decoration (those need a ChunkViewSource over the
//        3x3 neighbourhood: WorldGenerator::postProcessStructures + decorationPostProcessChunk).
//     4. Generating -> Generated; wrap in an owning BedrockChunk, snapshot() it on the worker thread (blocks,
//        biomes, height maps), release the LevelChunk, block-light it (lightChunk, block only) and deliver.
//   Features (Run 13, the internal porting notes "Run 13 (LOD content)"): base terrain for the batch + a 2-chunk ring (cached
//        per batch, generated in parallel by the batch's tasks); the last task to finish its base terrain decorates
//        the batch + a 1-chunk ring like vanilla's OverworldGenerator::decorationPostProcessChunk (the engine's
//        `decorateWorldGenPostProcess` with the vanilla chunk seed over the cell's 3x3, then the freeze/snow pass),
//        in vanilla's DecorationParity order, and delivers every chunk of the batch. The BlockSource is backed by a
//        private ChunkSource that serves ONLY the scratch chunks (never the live world's chunk map). A real
//        `ChunkViewSource` cannot be constructed from the SDK, so `decorationPostProcessChunk` itself is not called.
//        Run 15: before decoration, the vanilla structure pass (OverworldGenerator::structurePostProcessChunk's body:
//        postProcessStructureFeatures + postProcessStructures) runs over the batch + 2-chunk ring, so structures
//        whose blueprints loadChunk prepared appear in the LODs (dh/build/FeaturesBatchPlan.h for the plan).
//   PreExistingOnly: only chunks that are currently loaded (ChunkSource::getAvailableChunk), snapshotted on
//        the server thread; missing chunks are skipped (no error).
//   InternalServer: let the real ChunkSource generate and load the chunk (getOrLoadChunk on the server
//        thread), poll until it reaches ChunkState::Loaded (TTL 15 s), then snapshot on the server thread.
//
// Threads: scratch generation runs on this class's own FIFO worker threads (`scratchThreads`); only the LevelChunk
// construction/destruction hops to the server thread (Run 16: ONE construction task per batch, posted from queueBatch,
// the workers wait on a per-batch latch; destruction is batched in ~Batch);
// InternalServer / PreExistingOnly hop to the server thread (ll::thread::ServerThreadExecutor).
//
// Contract (IChunkGenerator.h): queue() never blocks; onChunk once per generated chunk with a shared_ptr<IChunk>
// the consumer may keep (never reused here); done(ok) exactly once per batch; cancelAll() makes pending
// batches finish with done(false). The destructor cancels and waits for in-flight tasks.

#include "bedrock_common/BedrockChunk.h"
#include "dh/build/ChunkCompare.h"
#include "dh/platform/IChunkGenerator.h"
#include "dh/platform/IExecutor.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Dimension;
class LevelChunk;

namespace dh::bedrock {

class BdsWorldGenerator final : public dh::IChunkGenerator {
public:
    /// `dimension` and `serializer` must outlive the generator; `genPool` runs the scratch generation.
    /// `scratchThreads`: workers for scratch generation (0 = auto: max(2, hardware_concurrency - 2);
    /// < 0 = legacy single worker with the whole task on the server thread).
    BdsWorldGenerator(
        Dimension&       dimension,
        BlockSerializer& serializer,
        dh::IExecutor&   genPool,
        int              scratchThreads = 0
    );
    static int defaultScratchThreads();
    ~BdsWorldGenerator() override;
    BdsWorldGenerator(const BdsWorldGenerator&)            = delete;
    BdsWorldGenerator& operator=(const BdsWorldGenerator&) = delete;

    void queue(
        std::int32_t       minChunkX,
        std::int32_t       minChunkZ,
        std::int32_t       widthChunks,
        dh::GenerationMode mode,
        dh::GenerationStep targetStep,
        ChunkCallback      onChunk,
        DoneCallback       done
    ) override;
    bool isBusy() const override;
    void cancelAll() override;
    /// Stops accepting work and cancels everything; call BEFORE the thread pools are shut down and joined.
    void close();
    /// close() + wait for in-flight tasks (including their onChunk/done callbacks) + join the workers. Call before
    /// anything the callbacks capture (the dh::WorldGenerator) is destroyed. After it, queue() fails fast with
    /// done(false).
    void stopWorkersAndWait();

    /// Compare mode (env DH_BEDROCK_GEN_COMPARE=1, the internal porting notes "Run 13"): `real` is a snapshot of a chunk the
    /// game generated; the same position is generated again through the Features path and the difference
    /// (dh::compareChunks) is logged per chunk plus a running total. At most 2 comparisons run at a time, the
    /// rest are skipped (counted). No-op when the mode is off.
    void               compareWithReal(std::shared_ptr<dh::IChunk> real);
    static bool        compareModeEnabled();

    std::uint64_t generatedChunks() const { return generated_.load(); }
    std::uint64_t failedChunks() const { return failed_.load(); }

private:
    struct Batch;
    struct TaskGuard;
    struct CellView;
    /// Compare mode: the delivered chunk after base terrain, after the structure pass, and final.
    using StagedCallback = std::function<void(
        std::shared_ptr<dh::IChunk> base,
        std::shared_ptr<dh::IChunk> afterStructures,
        std::shared_ptr<dh::IChunk> gen
    )>;
    void queueBatch(
        std::int32_t       minChunkX,
        std::int32_t       minChunkZ,
        std::int32_t       widthChunks,
        dh::GenerationMode mode,
        ChunkCallback      onChunk,
        DoneCallback       done,
        StagedCallback     onStaged
    );
    void                        runChunk(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz);
    void                        workerLoop();
    void                        stopWorkers();
    void                        noteChunkDone(
        long long ms,
        bool      okChunk,
        long long featMs       = -1,
        long long placed       = 0,
        long long structMs     = 0,
        long long structPlaced = 0,
        bool      sampled      = false
    );
    void                        startWorkersLocked();
    static long long            nowMs();
    void                        finishChunk(const std::shared_ptr<Batch>& batch, bool ok);
    std::shared_ptr<dh::IChunk> generateScratch(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz);
    /// Blocks until the batch's single server-thread construction task has run (Run 16: one hop per batch instead of
    /// one per cell). Polls ready, closing_ and cancelled (1-20 ms); never gives up on its own (a paused game does not tick
    /// the server), warns once per batch after 15 s. False = aborted (closing / cancelled).
    bool                        waitBatchCells(const std::shared_ptr<Batch>& batch);
    /// Features mode, one task: base terrain of the 5x5 around (cx, cz) (cached per batch). The batch's last task to
    /// get here runs decorateAndDeliver. False when the chunk's own base terrain failed.
    bool                        runFeatures(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz);
    std::shared_ptr<LevelChunk> baseCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z);
    /// dh::makeFeaturesBatchPlan: vanilla structure pass over the batch + 2-chunk ring, then vanilla decoration of the
    /// batch + 1-chunk ring in DecorationParity order, then snapshot + onChunk per batch chunk (never a ring chunk).
    void                        decorateAndDeliver(const std::shared_ptr<Batch>& batch);
    /// OverworldGenerator::structurePostProcessChunk minus the structure-reference bookkeeping: the vanilla chunk seed,
    /// postProcessStructureFeatures + postProcessStructures over the cell's 3x3 (pieces clipped to the cell).
    bool                        structureCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z);
    /// OverworldGenerator::decorationPostProcessChunk minus the legacy/tick parts: decorateWorldGenPostProcess with
    /// the vanilla chunk seed, then the freeze/snow pass. The 3x3 around (x, z) is the BlockSource's world.
    bool                        decorateCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z);
    std::shared_ptr<dh::IChunk>
    snapshotLoaded(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz, bool loadIfMissing);

    Dimension&       dimension_;
    BlockSerializer& serializer_;
    dh::IExecutor&   genPool_;

    mutable std::mutex                 mutex_;
    std::condition_variable            idleCv_;
    std::vector<std::weak_ptr<Batch>>  batches_;
    std::atomic<int>                   inFlightTasks_{0};
    std::atomic<std::uint64_t>         generated_{0}, failed_{0};
    std::shared_ptr<std::atomic<bool>> closing_ = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool>                  warnedFeatures_{false};
    bool                               featuresDisabled_ = false; // env DH_BEDROCK_NO_FEATURES: Features -> Surface
    bool                               structuresDisabled_ = false; // env DH_BEDROCK_NO_STRUCTURES: skip the structure pass

    // Own FIFO worker threads (the shared DH picker pool is sized for CPU-only work and cannot be oversubscribed
    // by tasks that wait on the server thread).
    int                               threads_          = 1;
    bool                              serverThreadOnly_ = false;
    std::mutex                        workMutex_;
    std::condition_variable           workCv_;
    std::deque<std::function<void()>> work_;
    bool                              stopWorkers_ = false, workersStarted_ = false;
    std::vector<std::thread>          workers_;
    std::atomic<long long>            statChunks_{0}, statFailed_{0}, statMs_{0}, lastStatMs_{0};
    std::atomic<long long>            statFeatChunks_{0}, statFeatMs_{0}, statPlaced_{0};
    std::atomic<long long>            statStructMs_{0}, statStructPlaced_{0};
    std::atomic<long long>            statSampled_{0}; ///< delivered chunks whose placed/structure counts were measured
    std::atomic<long long>            excludedCells_{0}; ///< structure cells that took the exclusion path (Run 15)
    std::atomic<bool>                 loggedFeatureNames_{false};

    // compare mode (compareWithReal)
    std::atomic<int>       compareInFlight_{0};
    std::atomic<long long> compareSkipped_{0};
    std::mutex             compareMutex_;
    dh::ChunkDiff          compareTotal_;
    long long              compareCount_ = 0;
};

} // namespace dh::bedrock
