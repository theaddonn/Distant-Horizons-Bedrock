#pragma once

// dh::IChunkGenerator for the BDS server DLL: produces chunks that are NOT loaded in the world.
//
// Strategy (spike, nothing here could be run offline - see docs/spikes/server-driver.md):
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
//   Features: as Surface, but for each requested chunk the 3x3 neighbourhood of base-terrain scratch LevelChunks
//        is generated (cached per batch, each cell once) and the centre is decorated with the engine's own
//        `WorldGenerator::decorateWorldGenPostProcess(biome, lc, BlockSource&, Random&)` over a BlockSource backed
//        by a private ChunkSource that serves ONLY those scratch chunks (never the live world's chunk map). A real
//        `ChunkViewSource` cannot be constructed from the SDK (no exported ctor), so
//        `decorationPostProcessChunk(ChunkViewSource&)` is not used. Structures (villages ...) are out of scope.
//        Decoration + snapshot of one batch are serialised (ring chunks are shared and mutated).
//   PreExistingOnly: only chunks that are currently loaded (ChunkSource::getAvailableChunk), snapshotted on
//        the server thread; missing chunks are skipped (no error).
//   InternalServer: let the real ChunkSource generate and load the chunk (getOrLoadChunk on the server
//        thread), poll until it reaches ChunkState::Loaded (TTL 15 s), then snapshot on the server thread.
//
// Threads: scratch generation runs on this class's own FIFO worker threads (`scratchThreads`); only the LevelChunk
// construction/destruction hops to the server thread;
// InternalServer / PreExistingOnly hop to the server thread (ll::thread::ServerThreadExecutor).
//
// Contract (IChunkGenerator.h): queue() never blocks; onChunk once per generated chunk with a shared_ptr<IChunk>
// the consumer may keep (never reused here); done(ok) exactly once per batch; cancelAll() makes pending
// batches finish with done(false). The destructor cancels and waits for in-flight tasks.

#include "bedrock_common/BedrockChunk.h"
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

    std::uint64_t generatedChunks() const { return generated_.load(); }
    std::uint64_t failedChunks() const { return failed_.load(); }

private:
    struct Batch;
    struct TaskGuard;
    void                        runChunk(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz);
    void                        workerLoop();
    void                        stopWorkers();
    void                        noteChunkDone(long long ms, bool okChunk, long long featMs = -1, long long placed = 0);
    void                        startWorkersLocked();
    static long long            nowMs();
    void                        finishChunk(const std::shared_ptr<Batch>& batch, bool ok);
    std::shared_ptr<dh::IChunk> generateScratch(std::int32_t cx, std::int32_t cz);
    /// Features mode: base terrain of the 3x3 scratch neighbourhood (cached per batch), decoration of the centre.
    std::shared_ptr<dh::IChunk> generateFeatures(
        const std::shared_ptr<Batch>& batch,
        std::int32_t                  cx,
        std::int32_t                  cz,
        long long&                    featMs,
        long long&                    placedOut
    );
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
};

} // namespace dh::bedrock
