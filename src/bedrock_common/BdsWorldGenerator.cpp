#include "bedrock_common/BdsWorldGenerator.h"

#include "bedrock_common/FileLogger.h"

#include "dh/build/DecorationOrder.h"
#include "dh/build/FeaturesBatchPlan.h"
#include "dh/build/LightingEngine.h"

#include "mc/util/Random.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkBlockPos.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/WeatherHelpers.h"
#include "mc/world/level/block/Block.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunkMetaDataDictionary.h"
#include "mc/world/level/storage/BlendingDataProvider.h"
#include "mc/world/level/levelgen/v2/StructureInstance.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/levelgen/WorldGenerator.h"
#include "mc/world/level/levelgen/structure/BoundingBox.h"

#include "ll/api/thread/ServerThreadExecutor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dh::bedrock {

namespace {

using namespace std::chrono_literals;

constexpr auto kInternalServerTtl = 15s;

/// Keeps the scratch LevelChunk alive until the wrapper has snapshotted it.
struct ChunkHolder {
    std::shared_ptr<LevelChunk> lc;
};

class ScratchChunk final : private ChunkHolder, public BedrockChunk {
public:
    ScratchChunk(std::shared_ptr<LevelChunk> chunk, BlockSerializer& serializer)
    : ChunkHolder{std::move(chunk)},
      BedrockChunk(*lc, serializer) {}

    /// snapshot() and hand the LevelChunk back (afterwards this wrapper never dereferences it); the caller
    /// destroys it on the thread that constructed it.
    std::shared_ptr<LevelChunk> freeze() {
        snapshot();
        return std::move(lc);
    }
};

/// A ChunkSource that serves ONLY the scratch chunks handed to it (the 3x3 decoration neighbourhood). Used as the
/// backing store of the decoration BlockSource, so feature placement can never see or touch the live world.
///
/// 2026-10-19 crash: ChunkSource's only constructor, ChunkSource(std::unique_ptr<ChunkSource> parent), dereferences
/// the parent without a null check (copies mChunkSide/mLevel/mDimension from it and stores it as mParent +
/// mOwnedParent). So the dimension's live chunk source is passed as a NON-OWNING parent: the destructor releases
/// mOwnedParent before ~ChunkSource runs, and every virtual the base would delegate to the parent is overridden
/// below so decoration never reaches the live world from a worker thread.
class ScratchRegionSource final : public ::ChunkSource {
public:
    ScratchRegionSource(::Dimension& dim, ::Level& level)
    : ::ChunkSource(std::unique_ptr<::ChunkSource>(&dim.getChunkSource())) {
        mChunkSide = 16;
        mLevel     = &level;
        mDimension = &dim;
    }
    ~ScratchRegionSource() override {
        (void)mOwnedParent.release(); // plain std::unique_ptr here (TypedStorage alias); never delete the live source
        mParent = nullptr;
    }
    ScratchRegionSource(ScratchRegionSource const&)            = delete;
    ScratchRegionSource& operator=(ScratchRegionSource const&) = delete;

    void add(const std::shared_ptr<LevelChunk>& lc, std::int32_t cx, std::int32_t cz) {
        if (lc) chunks_[key(cx, cz)] = lc;
    }

    // ---- chunk lookups: only the scratch neighbourhood exists ----
    std::shared_ptr<LevelChunk> getExistingChunk(::ChunkPos const& pos) override { return find(pos); }
    std::shared_ptr<LevelChunk> getOrLoadChunk(::ChunkPos const& pos, LoadMode, bool) override { return find(pos); }
    std::shared_ptr<LevelChunk> createNewChunk(::ChunkPos const&, LoadMode, bool) override { return nullptr; }
    std::shared_ptr<LevelChunk> getRandomChunk(::Random&) override { return nullptr; }
    bool                        isChunkKnown(::ChunkPos const& pos) override { return find(pos) != nullptr; }
    bool                        isChunkSaved(::ChunkPos const&) override { return false; }
    bool                        isWithinWorldLimit(::ChunkPos const&) const override { return true; }
    std::unordered_map<::ChunkPos, std::weak_ptr<::LevelChunk>> const* getChunkMap() const override {
        return &emptyMap(); // non-null: callers rarely check
    }
    std::unordered_map<::ChunkPos, std::weak_ptr<::LevelChunk>> const& getStorage() const override {
        return emptyMap();
    }

    // ---- everything else: inert (the base implementations delegate to mParent = the live chunk source) ----
    void shutdown() override {}
    bool isShutdownDone() override { return true; }
    bool structurePostProcessChunk(::ChunkViewSource&) override { return false; }
    bool decorationPostProcessChunk(::ChunkViewSource&) override { return false; }
    void checkAndReplaceChunk(::ChunkViewSource&, ::LevelChunk&) override {}
    bool verifyChunkNeedsNeighborAwareUpgrade(::LevelChunk&) override { return false; }
    void neighborAwareChunkUpgrade(::LevelChunk&, ::ChunkViewSource&) override {}
    void loadChunk(::LevelChunk&, bool) override {}
    void postProcessMobsAt(::BlockSource&, int, int, ::Random&) override {}
    void postProcessMobsAt(::BlockSource&, ::BoundingBox const&) const override {}
    void deleteAllChunkData(
        std::unordered_set<::ChunkPos>,
        std::function<void()>,
        std::shared_ptr<::ChunkDeletionMetadata>
    ) override {}
    void deleteStoredChunkData(
        std::unordered_set<::ChunkPos>,
        std::function<void()>,
        std::shared_ptr<::ChunkDeletionMetadata>
    ) override {}
    bool saveLiveChunk(::LevelChunk&) override { return false; }
    void writeEntityChunkTransfer(::LevelChunk&) override {}
    void writeEntityChunkTransfersToUnloadedChunk(
        ::ChunkKey const&,
        std::vector<::ActorUnloadedChunkTransferEntry> const&
    ) override {}
    void deserializeActorStorageToLevelChunk(::LevelChunk&) override {}
    void hintDiscardBatchBegin() override {}
    void hintDiscardBatchEnd() override {}
    // acquireDiscarded(unique_ptr<LevelChunk, LevelChunkFinalDeleter>) is NOT overridden: the SDK's deleter has no call
    // operator, so a by-value definition cannot compile; the base delegates to the live source, which decoration never
    // reaches (chunks are only discarded by the live chunk map).
    void compact() override {}
    void flushPendingDiscardedChunkWrites() override {}
    void flushThreadBatch() override {}
    void clearDeletedEntities() override {}
    bool canCreateViews() const override { return false; }
    std::unique_ptr<::BlendingDataProvider>           tryGetBlendingDataProvider() override { return nullptr; }
    std::shared_ptr<::LevelChunkMetaDataDictionary>   loadLevelChunkMetaDataDictionary() override { return nullptr; }
    void                                              setLevelChunk(std::shared_ptr<::LevelChunk>) override {}
    bool                                              canLaunchTasks() const override { return false; }
    bool                                              chunkPosNeedsBlending(::ChunkPos const&) override { return false; }
    void                                              _clearBlendingCache() override {}
    std::shared_ptr<::br::worldgen::StructureInstance>
    _tryGetOrLoadStructureInstanceAt(::ChunkPos const&, ::br::worldgen::Structure const&) override {
        return nullptr;
    }

private:
    static std::unordered_map<::ChunkPos, std::weak_ptr<::LevelChunk>> const& emptyMap() {
        static const std::unordered_map<::ChunkPos, std::weak_ptr<::LevelChunk>> empty;
        return empty;
    }
    static std::uint64_t key(std::int32_t x, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
    }
    std::shared_ptr<LevelChunk> find(::ChunkPos const& pos) const {
        auto it = chunks_.find(key(pos.x, pos.z));
        return it == chunks_.end() ? nullptr : it->second;
    }
    std::unordered_map<std::uint64_t, std::shared_ptr<LevelChunk>> chunks_;
};

/// Runs `fn` on the server thread and waits up to `ttl` for its result (R{} on timeout / abort). `abort` is checked
/// again on the server thread before `fn` runs (so a task that outlives the generator/dimension never touches them)
/// and polled while waiting, so shutdown never burns the full TTL.
template <class Fn>
auto onServerThread(Fn fn, std::chrono::milliseconds ttl, std::shared_ptr<std::atomic<bool>> abort) -> decltype(fn()) {
    using R   = decltype(fn());
    auto prom = std::make_shared<std::promise<R>>();
    auto fut  = prom->get_future();
    ll::thread::ServerThreadExecutor::getDefault().execute([prom, abort, fn = std::move(fn)]() mutable {
        try {
            if (abort->load()) {
                prom->set_value(R{});
                return;
            }
            prom->set_value(fn());
        } catch (...) {
            try {
                prom->set_value(R{});
            } catch (...) {}
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + ttl;
    while (fut.wait_for(50ms) != std::future_status::ready) {
        if (abort->load() || std::chrono::steady_clock::now() > deadline) return R{};
    }
    return fut.get();
}

void lightBlockOnly(BedrockChunk& chunk) {
    dh::LightingOptions lo;
    lo.maxSkyLight        = 15;
    lo.generateBlockLight = true;
    lo.generateSkyLight   = false; // sky light is baked per section by the updater (PORTING.md, J2)
    dh::lightChunk(chunk, {}, lo);
}

} // namespace

struct BdsWorldGenerator::Batch {
    /// One base-terrain scratch LevelChunk of the Features neighbourhood cache (generated once per batch).
    struct Cell {
        std::mutex                  m;
        bool                        done   = false;
        bool                        usable = false; ///< base terrain generated and the height range supported
        std::shared_ptr<LevelChunk> lc;
    };
    std::mutex                                                             cellsMutex;
    std::map<std::pair<std::int32_t, std::int32_t>, std::shared_ptr<Cell>> cells;
    std::mutex                         decoMutex; // decoration + snapshot are serialised per batch
    std::shared_ptr<std::atomic<bool>> closing;   // generator shutdown flag (null = never closing)
    ~Batch() {
        // The scratch LevelChunks were constructed on the server thread: destroy them there.
        std::vector<std::shared_ptr<LevelChunk>> lcs;
        for (auto& kv : cells)
            if (kv.second && kv.second->lc) lcs.push_back(std::move(kv.second->lc));
        if (lcs.empty()) return;
        // Closing: the Dimension may be gone by the time the server thread runs this; leak instead.
        if (closing && closing->load()) {
            for (auto& l : lcs) (void)new std::shared_ptr<LevelChunk>(std::move(l));
            return;
        }
        ll::thread::ServerThreadExecutor::getDefault().execute([lcs = std::move(lcs)]() mutable { lcs.clear(); });
    }
    std::shared_ptr<Cell> cell(std::int32_t x, std::int32_t z) {
        std::lock_guard lock(cellsMutex);
        auto&           c = cells[{x, z}];
        if (!c) c = std::make_shared<Cell>();
        return c;
    }
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ok{true};
    std::atomic<int>  remaining{0};
    GenerationMode    mode = GenerationMode::Surface;
    // Features: the requested square, the tasks that still have to finish their base terrain (the last one
    // decorates and delivers the whole batch) and the summed base-terrain wall time of the tasks.
    std::int32_t           minX = 0, minZ = 0, width = 0;
    std::atomic<int>       featurePending{0};
    std::atomic<long long> baseMsSum{0};
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    /// The cell's LevelChunk if its base terrain exists (never creates a cell).
    std::shared_ptr<LevelChunk> existing(std::int32_t x, std::int32_t z) {
        std::shared_ptr<Cell> c;
        {
            std::lock_guard lock(cellsMutex);
            auto            it = cells.find({x, z});
            if (it == cells.end()) return nullptr;
            c = it->second;
        }
        std::lock_guard lock(c->m);
        return c->usable ? c->lc : nullptr;
    }
    ChunkCallback     onChunk;
    DoneCallback      done;
    /// Compare mode only: called instead of onChunk with the delivered chunk after base terrain, after the structure
    /// pass and final (compareChunksStaged).
    StagedCallback    onStaged;
};

int BdsWorldGenerator::defaultScratchThreads() {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(2, hw - 2);
}

BdsWorldGenerator::BdsWorldGenerator(
    Dimension&       dimension,
    BlockSerializer& serializer,
    dh::IExecutor&   genPool,
    int              scratchThreads
)
: dimension_(dimension),
  serializer_(serializer),
  genPool_(genPool) {
    // 0 = auto; < 0 = legacy: one worker, everything on the server thread (fallback if off-thread loadChunk
    // misbehaves).
    serverThreadOnly_ = scratchThreads < 0;
    threads_          = serverThreadOnly_ ? 1 : (scratchThreads == 0 ? defaultScratchThreads() : scratchThreads);
    lastStatMs_.store(nowMs());
    featuresDisabled_ = [] {
        const char* e = std::getenv("DH_BEDROCK_NO_FEATURES");
        return e != nullptr && std::string(e) == "1";
    }();
    structuresDisabled_ = [] {
        const char* e = std::getenv("DH_BEDROCK_NO_STRUCTURES");
        return e != nullptr && std::string(e) == "1";
    }();
    DH_LOG_I(
        "world gen: configured %d scratch worker thread(s)%s (started on first use)",
        threads_,
        serverThreadOnly_ ? " (legacy: whole task on the server thread)" : ""
    );
}

void BdsWorldGenerator::startWorkersLocked() {
    if (workersStarted_ || stopWorkers_) return;
    workersStarted_ = true;
    DH_LOG_I("world gen: starting %d scratch worker thread(s)", threads_);
    for (int i = 0; i < threads_; ++i) workers_.emplace_back([this] { workerLoop(); });
}

long long BdsWorldGenerator::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void BdsWorldGenerator::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(workMutex_);
            workCv_.wait(lock, [&] { return stopWorkers_ || !work_.empty(); });
            if (work_.empty()) return; // stopping and drained
            job = std::move(work_.front());
            work_.pop_front();
        }
        try {
            job(); // the closure's TaskGuard releases the in-flight slot
        } catch (...) {
            DH_LOG_E("world gen: exception escaped a generation job");
        }
    }
}

void BdsWorldGenerator::close() {
    closing_->store(true);
    cancelAll();
}

void BdsWorldGenerator::stopWorkers() {
    {
        std::lock_guard lock(workMutex_);
        stopWorkers_ = true;
    }
    workCv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
}

/// Owned by the pool task's closure: whether the task runs or is dropped (ThreadPools::shutdown() drops queued
/// tasks), its destruction releases the in-flight slot and, if the task never ran, completes the chunk as failed.
struct BdsWorldGenerator::TaskGuard {
    BdsWorldGenerator*     self;
    std::shared_ptr<Batch> batch;
    bool                   ran = false;
    // Constructed in place only: a copied/moved temporary would run the destructor (double release + false failure).
    TaskGuard(BdsWorldGenerator* s, std::shared_ptr<Batch> b) : self(s), batch(std::move(b)) {}
    TaskGuard(const TaskGuard&)            = delete;
    TaskGuard& operator=(const TaskGuard&) = delete;
    ~TaskGuard() {
        if (!ran) self->finishChunk(batch, false);
        std::lock_guard lock(self->mutex_);
        const int       left = --self->inFlightTasks_;
        if (left < 0) DH_LOG_E("world gen: in-flight counter went negative (%d): double release", left);
        if (left <= 0) self->idleCv_.notify_all();
    }
};

BdsWorldGenerator::~BdsWorldGenerator() { stopWorkersAndWait(); }

void BdsWorldGenerator::stopWorkersAndWait() {
    close();
    {
        std::unique_lock lock(mutex_);
        // Tasks capture `this`: wait for them (they fail fast once closing_). Server-thread hops poll closing_, so
        // this is safe even when called on the server thread. Bounded as a last resort.
        idleCv_.wait_for(lock, 30s, [&] { return inFlightTasks_.load() == 0; });
    }
    stopWorkers();
}

bool BdsWorldGenerator::isBusy() const {
    // Java: the queue itself bounds in-flight tasks; also stay out of the way while the server thread is the
    // only way to progress (InternalServer waits there).
    return inFlightTasks_.load() > std::max(64, threads_ * 4);
}

void BdsWorldGenerator::cancelAll() {
    std::vector<std::shared_ptr<Batch>> live;
    {
        std::lock_guard lock(mutex_);
        for (auto& w : batches_)
            if (auto b = w.lock()) live.push_back(std::move(b));
        batches_.clear();
    }
    for (auto& b : live) b->cancelled.store(true);
    // Queued tasks notice `cancelled` when they run and complete the batch with done(false).
}

void BdsWorldGenerator::queue(
    std::int32_t       minChunkX,
    std::int32_t       minChunkZ,
    std::int32_t       widthChunks,
    dh::GenerationMode mode,
    dh::GenerationStep /*targetStep*/,
    ChunkCallback onChunk,
    DoneCallback  done
) {
    queueBatch(minChunkX, minChunkZ, widthChunks, mode, std::move(onChunk), std::move(done), nullptr);
}

void BdsWorldGenerator::queueBatch(
    std::int32_t       minChunkX,
    std::int32_t       minChunkZ,
    std::int32_t       widthChunks,
    dh::GenerationMode mode,
    ChunkCallback      onChunk,
    DoneCallback       done,
    StagedCallback     onStaged
) {
    if (widthChunks <= 0 || closing_->load()) {
        if (done) done(false);
        return;
    }
    if (mode == GenerationMode::Features && !warnedFeatures_.exchange(true)) {
        if (featuresDisabled_)
            DH_LOG_W("world gen: Features mode disabled by DH_BEDROCK_NO_FEATURES, generating Surface (base terrain)");
        else
            DH_LOG_I(
                "world gen: Features mode: base terrain of the batch + 2-chunk ring, vanilla structure pass "
                "(postProcessStructureFeatures + postProcessStructures, vanilla seed) of the batch + 2-chunk ring%s, "
                "vanilla decoration (decorateWorldGenPostProcess + freeze/snow pass, vanilla seed) of the batch + "
                "1-chunk ring in DecorationParity order; only the batch is delivered; slower than Surface",
                structuresDisabled_ ? " DISABLED by DH_BEDROCK_NO_STRUCTURES" : ""
            );
    }

    auto batch       = std::make_shared<Batch>();
    batch->closing   = closing_;
    batch->mode      = mode;
    batch->onChunk   = std::move(onChunk);
    batch->done      = std::move(done);
    batch->onStaged  = std::move(onStaged);
    batch->remaining = widthChunks * widthChunks;
    batch->minX           = minChunkX;
    batch->minZ           = minChunkZ;
    batch->width          = widthChunks;
    batch->featurePending = widthChunks * widthChunks;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(batches_, [](const std::weak_ptr<Batch>& w) { return w.expired(); });
        batches_.push_back(batch);
    }
    for (std::int32_t dz = 0; dz < widthChunks; ++dz) {
        for (std::int32_t dx = 0; dx < widthChunks; ++dx) {
            const std::int32_t cx = minChunkX + dx, cz = minChunkZ + dz;
            ++inFlightTasks_;
            auto guard = std::make_shared<TaskGuard>(this, batch);
            // If rejected, the closure (and with it the guard) is destroyed and completes the chunk as failed.
            // FIFO across batches: a section's 16 chunks are picked up together before the next section's.
            std::lock_guard lock(workMutex_);
            // Workers already stopped (queue() racing stopWorkersAndWait): nothing would ever run the task. The lock
            // is released before `guard` dies, whose destructor completes the chunk as failed.
            if (stopWorkers_) continue;
            startWorkersLocked();
            work_.emplace_back([this, batch, cx, cz, guard] {
                guard->ran = true;
                runChunk(batch, cx, cz);
            });
        }
    }
    workCv_.notify_all();
}

void BdsWorldGenerator::noteChunkDone(
    long long ms,
    bool      okChunk,
    long long featMs,
    long long placed,
    long long structMs,
    long long structPlaced
) {
    (okChunk ? statChunks_ : statFailed_).fetch_add(1);
    statMs_.fetch_add(ms);
    if (featMs >= 0) {
        statFeatChunks_.fetch_add(1);
        statFeatMs_.fetch_add(featMs);
        statPlaced_.fetch_add(placed);
        statStructMs_.fetch_add(structMs);
        statStructPlaced_.fetch_add(structPlaced);
    }
    const long long now  = nowMs();
    long long       last = lastStatMs_.load();
    if (now - last < 10000 || !lastStatMs_.compare_exchange_strong(last, now)) return;
    const long long n   = statChunks_.exchange(0);
    const long long nf  = statFailed_.exchange(0);
    const long long sum = statMs_.exchange(0);
    const long long tot = n + nf;
    const long long fn  = statFeatChunks_.exchange(0);
    const long long fms = statFeatMs_.exchange(0);
    const long long pl  = statPlaced_.exchange(0);
    const long long sms = statStructMs_.exchange(0);
    const long long spl = statStructPlaced_.exchange(0);
    if (tot > 0)
        DH_LOG_I(
            "world gen: %lld chunks in the last 10 s (avg %lld ms%s, in flight %d, threads %d; ok %lld, failed %lld)"
            "%s",
            n,
            sum / tot,
            fn > 0 ? " incl. features" : "",
            inFlightTasks_.load(),
            threads_,
            n,
            nf,
            fn > 0 ? (std::string(", features avg ") + std::to_string(fms / fn) + " ms (structures "
                      + std::to_string(sms / fn) + " ms), placedBlocks " + std::to_string(pl) + " ("
                      + std::to_string(pl / fn) + "/chunk), structureBlocks " + std::to_string(spl))
                         .c_str()
                   : ""
        );
}

void BdsWorldGenerator::finishChunk(const std::shared_ptr<Batch>& batch, bool ok) {
    if (!ok) batch->ok.store(false);
    if (batch->remaining.fetch_sub(1) == 1) {
        const bool allOk = batch->ok.load() && !batch->cancelled.load();
        if (batch->done) batch->done(allOk);
    }
}

void BdsWorldGenerator::runChunk(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz) {
    if (batch->cancelled.load() || closing_->load()) {
        finishChunk(batch, false);
        return;
    }
    bool       ok     = true;
    const auto taskT0 = std::chrono::steady_clock::now();
    long long  taskMs = 0;
    long long  featMs = -1, placed = 0;
    try {
        std::shared_ptr<dh::IChunk> chunk;
        switch (batch->mode) {
        case GenerationMode::PreExistingOnly:
            chunk = snapshotLoaded(batch, cx, cz, /*loadIfMissing=*/false);
            break;
        case GenerationMode::InternalServer:
            chunk = snapshotLoaded(batch, cx, cz, /*loadIfMissing=*/true);
            ok    = chunk != nullptr;
            break;
        case GenerationMode::Features:
            if (!featuresDisabled_) {
                ok = runFeatures(batch, cx, cz); // the batch's last task decorates and delivers every chunk
                break;
            }
            [[fallthrough]];
        case GenerationMode::Surface:
        default:
            chunk = generateScratch(cx, cz);
            ok    = chunk != nullptr;
            break;
        }
        taskMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - taskT0).count();
        if (chunk && !batch->cancelled.load() && batch->onStaged) {
            batch->onStaged(nullptr, nullptr, std::move(chunk)); // compare mode with DH_BEDROCK_NO_FEATURES: unstaged
        } else if (chunk && !batch->cancelled.load() && batch->onChunk) {
            ++generated_;
            noteChunkDone(taskMs, true, featMs, placed);
            batch->onChunk(std::move(chunk));
        } else if (!chunk && !ok) {
            ++failed_;
            noteChunkDone(taskMs, false);
        }
    } catch (...) {
        ok = false;
        ++failed_;
        DH_LOG_E("world gen: exception swallowed for chunk %d,%d", cx, cz);
    }
    finishChunk(batch, ok);
}

std::shared_ptr<dh::IChunk> BdsWorldGenerator::generateScratch(std::int32_t cx, std::int32_t cz) {
    // Per-task outcome log for the first 10 tasks (proves the fix / shows hangs in game).
    static std::atomic<int> taskSeq{0};
    const int               seq   = taskSeq.fetch_add(1);
    const bool              logIt = seq < 10;
    const auto              t0    = std::chrono::steady_clock::now();
    auto                    ms    = [&] {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
        );
    };

    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    if (gen == nullptr) {
        DH_LOG_E("world gen: dimension has no WorldGenerator");
        return nullptr;
    }
    const ::ChunkPos cp(cx, cz);

    // WorldGenerator does not override ChunkSource::createNewChunk (only MainChunkSource, WorldLimitChunkSource,
    // ChunkViewSource and NetworkChunkSource do); the base implementation forwards to mParent, which a generator
    // does not have -> always null. Do not call it. Build the scratch LevelChunk directly instead.
    //
    ::Dimension*     dim = &dimension_;
    BlockSerializer* ser = &serializer_;
    struct Diag {
        bool heightOk     = false;
        int  stateBefore  = -1;
        int  stateAfter   = -1;
        bool toGenerating = false;
        bool toGenerated  = false;
    };
    auto diag = std::make_shared<Diag>();

    auto construct = [dim, cp]() -> std::shared_ptr<LevelChunk> {
        return std::shared_ptr<LevelChunk>(new LevelChunk(
            *dim,
            cp,
            /*readOnly=*/false,
            SubChunkInitMode::All,
            /*initializeMetaData=*/false,
            LevelChunkBlockActorStorage::TrackingMode::None
        ));
    };
    // Generate + snapshot; returns the wrapper (LevelChunk already released into `leftover` for destruction).
    auto generate = [gen, ser, cp, diag](std::shared_ptr<LevelChunk> lc, std::shared_ptr<LevelChunk>& leftover)
        -> std::shared_ptr<ScratchChunk> {
        diag->heightOk = BedrockChunk::heightSupported(*lc);
        if (!diag->heightOk) {
            leftover = std::move(lc);
            return nullptr;
        }
        diag->stateBefore  = static_cast<int>(lc->mLoadState->load());
        diag->toGenerating = lc->tryChangeState(ChunkState::Unloaded, ChunkState::Generating);
        gen->loadChunk(*lc, /*forceImmediateReplacementDataLoad=*/false);
        diag->toGenerated = lc->tryChangeState(ChunkState::Generating, ChunkState::Generated);
        diag->stateAfter  = static_cast<int>(lc->mLoadState->load());
        auto w            = std::make_shared<ScratchChunk>(std::move(lc), *ser);
        leftover          = w->freeze();
        return w;
    };

    std::shared_ptr<ScratchChunk> wrapped;
    if (serverThreadOnly_) {
        // Legacy path: everything on the server thread (serialised).
        wrapped = onServerThread(
            [construct, generate]() -> std::shared_ptr<ScratchChunk> {
                std::shared_ptr<LevelChunk> leftover;
                auto                        w = generate(construct(), leftover);
                return w; // leftover destroyed here, on the server thread
            },
            kInternalServerTtl,
            closing_
        );
    } else {
        // Parallel path. Construction (ctor reads Dimension/Level state) and destruction stay on the server
        // thread; loadChunk and the snapshot run on this worker: WorldGenerator's per-thread state
        // (mThreadLocalHeightmapProvider is InstancedThreadLocalValue, generatorHelpersPool InstancedThreadLocal)
        // is created lazily per calling thread, which is how vanilla runs it on its generation workers.
        auto lc = onServerThread(construct, kInternalServerTtl, closing_);
        if (lc) {
            std::shared_ptr<LevelChunk> leftover;
            wrapped = generate(std::move(lc), leftover);
            if (leftover)
                ll::thread::ServerThreadExecutor::getDefault().execute([leftover = std::move(leftover)]() mutable {
                    leftover.reset();
                });
        }
    }
    if (!wrapped) {
        if (logIt)
            DH_LOG_W(
                "world gen task %d (%d,%d): FAILED after %lld ms (null; heightSupported=%d)",
                seq,
                cx,
                cz,
                ms(),
                diag->heightOk ? 1 : 0
            );
        return nullptr;
    }

    // Validate: a chunk with no solid block is a failed generation, not terrain.
    long long nonAir = 0;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (std::int32_t y = wrapped->minNonEmptyHeight(); y < wrapped->maxNonEmptyHeight(); ++y)
                if (!wrapped->blockAt(x, y, z)->isAir) ++nonAir;
    const std::int32_t   topY   = wrapped->solidHeightMapValue(8, 8);
    const dh::BlockInfo* topBlk = wrapped->blockAt(8, topY - 1, 8);
    if (logIt)
        DH_LOG_I(
            "world gen task %d (%d,%d): %lld ms, state %d->%d (toGenerating=%d toGenerated=%d), nonAir=%lld, "
            "top(8,8)=%s at y=%d, heightSupported=1",
            seq,
            cx,
            cz,
            ms(),
            diag->stateBefore,
            diag->stateAfter,
            diag->toGenerating ? 1 : 0,
            diag->toGenerated ? 1 : 0,
            nonAir,
            topBlk ? topBlk->serial.c_str() : "?",
            topY - 1
        );
    if (nonAir == 0) return nullptr;
    lightBlockOnly(*wrapped);
    return wrapped;
}

// --- Features mode (docs/PORTING.md "Run 13 (LOD content)") ---------------------------------------------------
//
// Vanilla (static RE of 1.26.51, OverworldGenerator::decorationPostProcessChunk @0xc594e80 and its scheduler
// @0x29db3c0): a chunk is decorated over a 3x3 ChunkViewSource centred on it, so features spill into all eight
// neighbours, and a chunk only becomes final after all eight neighbours were decorated too. The scheduler orders the
// decoration by chunk parity (ChunkStateNeighborDependency::DecorationParity tables @0xe840350..68): (even, even)
// chunks first, then (odd, even) once their x neighbours are done, (even, odd) once the rows above/below are done,
// (odd, odd) last. We therefore build base terrain for the batch + 2 rings (every decorated cell needs its 3x3),
// decorate the batch + 1 ring in that order, and deliver the batch. All tasks of a batch generate base terrain in
// parallel; the last one to finish does the (serial) decoration and the delivery, so no worker ever waits.

bool BdsWorldGenerator::runFeatures(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz) {
    bool       ok = true;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        // centre first: it is the one whose failure fails the chunk
        if (!baseCell(batch, cx, cz)) ok = false;
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                if ((dx == 0 && dz == 0) || batch->cancelled.load() || closing_->load()) continue;
                (void)baseCell(batch, cx + dx, cz + dz);
            }
    } catch (...) {
        ok = false;
        DH_LOG_E("world gen: exception in base terrain for chunk %d,%d", cx, cz);
    }
    batch->baseMsSum.fetch_add(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
    );
    if (batch->featurePending.fetch_sub(1) == 1) {
        try {
            decorateAndDeliver(batch);
        } catch (...) {
            batch->ok.store(false);
            DH_LOG_E("world gen: exception in batch decoration at %d,%d", batch->minX, batch->minZ);
        }
    }
    return ok;
}

std::shared_ptr<LevelChunk>
BdsWorldGenerator::baseCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z) {
    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    if (gen == nullptr) return nullptr;
    ::Dimension*    dim  = &dimension_;
    auto            cell = batch->cell(x, z);
    std::lock_guard lock(cell->m);
    if (cell->done) return cell->usable ? cell->lc : nullptr;
    cell->done = true;
    const ::ChunkPos cp(x, z);
    auto             lc = onServerThread(
        [dim, cp]() -> std::shared_ptr<LevelChunk> {
            return std::shared_ptr<LevelChunk>(new LevelChunk(
                *dim,
                cp,
                /*readOnly=*/false,
                SubChunkInitMode::All,
                /*initializeMetaData=*/false,
                LevelChunkBlockActorStorage::TrackingMode::None
            ));
        },
        kInternalServerTtl,
        closing_
    );
    if (!lc) {
        // hop timed out / aborted: let the next task that needs this cell try again
        cell->done = false;
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 10) DH_LOG_W("world gen: base chunk %d,%d not constructed (server-thread hop)", x, z);
        return nullptr;
    }
    if (!BedrockChunk::heightSupported(*lc)) {
        cell->lc = std::move(lc); // keep it so ~Batch destroys it on the server thread (never handed out)
        return nullptr;
    }
    (void)lc->tryChangeState(ChunkState::Unloaded, ChunkState::Generating);
    gen->loadChunk(*lc, /*forceImmediateReplacementDataLoad=*/false);
    (void)lc->tryChangeState(ChunkState::Generating, ChunkState::Generated);
    cell->lc     = std::move(lc);
    cell->usable = true;
    return cell->lc;
}

namespace {

long long countNonAir(dh::IChunk& w) {
    long long n = 0;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (std::int32_t y = w.minNonEmptyHeight(); y < w.maxNonEmptyHeight(); ++y)
                if (!w.blockAt(x, y, z)->isAir) ++n;
    return n;
}

} // namespace

namespace {

/// The 32-bit per-chunk seed vanilla derives for both post-process passes (structurePostProcessChunk @0xc594440 and
/// decorationPostProcessChunk @0xc594e80 compute it identically): r.setSeed(levelSeed); xs = r.nextInt()/2*2+1;
/// zs = r.nextInt()/2*2+1; seed = levelSeed ^ (cx*xs + cz*zs).
std::uint32_t vanillaChunkSeed(std::uint32_t levelSeed, std::int32_t x, std::int32_t z) {
    ::Random            seedRng(levelSeed, true);
    const std::uint32_t xs = dh::decorationSeedMultiplier(seedRng.nextInt());
    const std::uint32_t zs = dh::decorationSeedMultiplier(seedRng.nextInt());
    return dh::decorationChunkSeed(levelSeed, xs, zs, x, z);
}

} // namespace

/// The 3x3 scratch view around one cell as a BlockSource (vanilla: a BlockSource over the cell's ChunkViewSource;
/// missing neighbours are simply absent). Never sees the live world's chunks.
struct BdsWorldGenerator::CellView {
    ScratchRegionSource src;
    ::BlockSource       bs;
    CellView(::Dimension& dim, ::Level& level, const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z)
    : src(dim, level),
      bs(level,
         dim,
         (fill(batch, x, z), src),
         /*publicSource=*/false,
         /*allowUnpopulatedChunks=*/true,
         // vanilla passes allowClientTickingChanges=true for both passes; false is kept here (proven in game for
         // decoration, nothing ticks on scratch chunks)
         /*allowClientTickingChanges=*/false,
         /*markChangedBlocksForSaving=*/false) {}

private:
    void fill(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (auto n = batch->existing(x + dx, z + dz)) src.add(n, x + dx, z + dz);
    }
};

bool BdsWorldGenerator::structureCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z) {
    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    auto              lc  = batch->existing(x, z);
    if (gen == nullptr || !lc || gen->mStructureFeatureRegistry.get() == nullptr) return false;
    ::Level& level = static_cast<::Level&>(dimension_.mLevel);
    CellView view(dimension_, level, batch, x, z);

    // OverworldGenerator::structurePostProcessChunk @0xc594440 (shared by every overworld generator vtable, vslot 9):
    // per-thread Random seeded with the level seed, the chunk seed above, then
    //   postProcessStructureFeatures(bs, random, seed, cx, cz)  (0x231b2b0: legacy StructureFeature starts, each
    //       feature reseeds the Random with `seed`; pieces are clipped to the centre chunk's bounding box)
    //   postProcessStructures(bs, random, cx, cz)              (0x231c5e0: v2 StructureInstances of the
    //       StructureCache, own Xoroshiro seeded from the 64-bit level seed; the Random is unused)
    // Both only READ the blueprints/instances that loadChunk (prepareStructureFeatureBlueprints 0x231b5e0,
    // preProcessStructures 0x231b7a0) created in the live generator's shared caches; nothing is created here.
    // The tail loop that records structure references in the chunk (saved-data only) is not reproduced.
    const std::uint32_t levelSeed = level.getSeed();
    const std::uint32_t seed      = vanillaChunkSeed(levelSeed, x, z);
    ::Random            rnd(levelSeed, true);
    (void)rnd.nextInt();
    (void)rnd.nextInt();
    gen->postProcessStructureFeatures(view.bs, rnd, seed, x, z);
    gen->postProcessStructures(view.bs, rnd, x, z);
    return true;
}

bool BdsWorldGenerator::decorateCell(const std::shared_ptr<Batch>& batch, std::int32_t x, std::int32_t z) {
    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    auto              lc  = batch->existing(x, z);
    if (gen == nullptr || !lc) return false;
    ::Level& level = static_cast<::Level&>(dimension_.mLevel);

    // Vanilla: BlockSource over the 3x3 view (vanilla also prunes neighbours that are already past
    // CheckingForReplacementData; within freshly generated terrain none can be, see PORTING.md "Run 15").
    CellView      view(dimension_, level, batch, x, z);
    ::BlockSource& bs = view.bs;

    // Vanilla chunk seed (decorationPostProcessChunk @0xc59536c..0xc5954e5, all 32-bit).
    // BiomeDecorationSystem derives every feature rule's own seed from this one (hash_combine with the rule id).
    ::Random rnd(vanillaChunkSeed(level.getSeed(), x, z), true);

    // The Multinoise override ignores the biome argument (it decorates per unique biome of the chunk).
    const int      minY  = dimension_.mHeightRange->mMin;
    const short    seaY  = static_cast<short>(std::clamp(63 - minY, 0, dimension_.mHeightRange->mMax - minY - 1));
    const ::Biome& biome = lc->getBiome(::ChunkBlockPos{8, ChunkLocalHeight{seaY}, 8});
    gen->decorateWorldGenPostProcess(biome, *lc, bs, rnd);

    // Freeze/snow pass (decorationPostProcessChunk @0xc595fd9..0xc59619c), right after the features: per column of
    // this chunk, ice on freezable water below the rain-top block, a snow layer on it where it snows.
    // Looked up per cell, never cached in a static: the block registry is rebuilt on every world join, so a cached
    // Block* dangles after leaving and re-entering a world (crash in BlockSource::setBlock, 2026-10-20).
    const ::Block* ice  = ::Block::tryGetFromRegistry(::HashedString("minecraft:ice")).as_ptr();
    const ::Block* snow = ::Block::tryGetFromRegistry(::HashedString("minecraft:snow_layer")).as_ptr();
    const short           y0   = static_cast<short>(-minY); // vanilla passes local y = -minY (world y 0)
    for (int bx = 0; bx < 16; ++bx)
        for (int bz = 0; bz < 16; ++bz) {
            const ::BlockPos top = lc->getTopRainBlockPos(
                ::ChunkBlockPos{static_cast<uchar>(bx), ChunkLocalHeight{y0}, static_cast<uchar>(bz)}
            );
            const ::BlockPos below{top.x, top.y - 1, top.z};
            if (ice != nullptr && WeatherHelpers::shouldFreeze(bs, below, false))
                bs.setBlockAndRetainCompatibleBlockActor(below, *ice, 2);
            if (snow != nullptr && WeatherHelpers::shouldSnow(bs, top))
                bs.setBlockAndRetainCompatibleBlockActor(top, *snow, 2);
        }
    return true;
}

void BdsWorldGenerator::decorateAndDeliver(const std::shared_ptr<Batch>& batch) {
    if (batch->cancelled.load() || closing_->load()) {
        batch->ok.store(false); // nothing delivered: never report done(true)
        return;
    }
    static std::atomic<int> batchSeq{0};
    const bool              logIt = batchSeq.fetch_add(1) < 10;
    const auto              f0    = std::chrono::steady_clock::now();
    const std::int32_t      w = batch->width, x0 = batch->minX, z0 = batch->minZ;
    std::lock_guard         deco(batch->decoMutex);
    // Run 15 (docs/PORTING.md): vanilla's state machine runs the structure pass of a chunk once its 3x3 has base
    // terrain, decorates it once all eight neighbours are structure-processed (plus DecorationParity), and a chunk is
    // final once its whole 3x3 is decorated. The plan reproduces that for the batch; only batch chunks are delivered,
    // after the last decoration, so no decoration ever spills into a chunk that was already delivered.
    const dh::FeaturesBatchPlan plan   = dh::makeFeaturesBatchPlan(x0, z0, w);
    const bool                  staged = static_cast<bool>(batch->onStaged);

    auto snap = [&](std::int32_t x, std::int32_t z) -> std::shared_ptr<ScratchChunk> {
        auto lc = batch->existing(x, z);
        if (!lc) return nullptr;
        auto s = std::make_shared<ScratchChunk>(lc, serializer_);
        (void)s->freeze(); // the batch keeps the LevelChunk alive
        return s;
    };
    // non-air count of the delivered chunks before the structure pass / before decoration (placedBlocks stats and
    // no-op detector); compare mode keeps the staged snapshots themselves
    const std::size_t                          nDeliver = plan.deliver.size();
    std::vector<long long>                     before(nDeliver, 0), afterStruct(nDeliver, 0);
    std::vector<std::shared_ptr<ScratchChunk>> baseSnap(staged ? nDeliver : 0), structSnap(staged ? nDeliver : 0);
    for (std::size_t i = 0; i < nDeliver; ++i)
        if (auto s = snap(plan.deliver[i].x, plan.deliver[i].z)) {
            before[i] = countNonAir(*s);
            if (staged) baseSnap[i] = std::move(s);
        }

    auto aborted = [&] {
        if (!batch->cancelled.load() && !closing_->load()) return false;
        batch->ok.store(false);
        return true;
    };

    // 1. structure pass over the batch + 2 rings (every decorated chunk's neighbours); order-free: each cell's pieces
    //    are clipped to the cell itself
    const auto            s0  = std::chrono::steady_clock::now();
    const ::WorldGenerator* wg = dimension_.mWorldGenerator;
    const bool noRegistry = wg == nullptr || wg->mStructureFeatureRegistry.get() == nullptr;
    int        structured = 0, structMissing = 0, structThrew = 0;
    if (!structuresDisabled_ && !noRegistry)
        for (const auto& c : plan.structures) {
            if (aborted()) return;
            try {
                if (structureCell(batch, c.x, c.z)) ++structured;
                else ++structMissing;
            } catch (...) {
                ++structThrew;
            }
        }
    const long long structMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s0).count();
    if (structThrew > 0)
        DH_LOG_W("world gen: structure pass threw in %d chunk(s) of batch (%d,%d)", structThrew, x0, z0);
    for (std::size_t i = 0; i < nDeliver; ++i)
        if (auto s = snap(plan.deliver[i].x, plan.deliver[i].z)) {
            afterStruct[i] = countNonAir(*s);
            if (staged) structSnap[i] = std::move(s);
        }

    // 2. decoration of the batch + 1 ring in DecorationParity order
    const auto d0        = std::chrono::steady_clock::now();
    int        decorated = 0, missing = 0, threw = 0;
    for (const auto& c : plan.decorate) {
        if (aborted()) return;
        try {
            if (decorateCell(batch, c.x, c.z)) ++decorated;
            else ++missing;
        } catch (...) {
            ++threw;
        }
    }
    const long long decoMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - d0).count();
    const long long featMs = // structure pass + decoration + the stats snapshots
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - f0).count();
    if (threw > 0) DH_LOG_W("world gen: decoration threw in %d chunk(s) of batch (%d,%d)", threw, x0, z0);

    // 3. deliver the batch (never a ring chunk)
    const long long n       = static_cast<long long>(nDeliver);
    const long long perMs   = (batch->baseMsSum.load() + featMs) / std::max(1LL, n);
    long long       placedT = 0, structPlacedT = 0;
    int             okCount = 0;
    for (std::size_t i = 0; i < nDeliver; ++i) {
        auto wrapped = snap(plan.deliver[i].x, plan.deliver[i].z);
        if (!wrapped) {
            batch->ok.store(false); // the chunk's own task already counted the failure
            continue;
        }
        const long long nonAir = countNonAir(*wrapped);
        if (nonAir == 0) {
            batch->ok.store(false);
            ++failed_;
            noteChunkDone(perMs, false);
            continue;
        }
        const long long placed       = nonAir - before[i];
        const long long structPlaced = afterStruct[i] - before[i];
        placedT += placed;
        structPlacedT += structPlaced;
        lightBlockOnly(*wrapped);
        if (batch->cancelled.load()) continue;
        if (staged) {
            ++okCount;
            batch->onStaged(baseSnap[i], structSnap[i], std::move(wrapped));
            continue;
        }
        if (!batch->onChunk) continue;
        ++generated_;
        ++okCount;
        noteChunkDone(perMs, true, featMs / std::max(1LL, n), placed, structMs / std::max(1LL, n), structPlaced);
        batch->onChunk(std::move(wrapped));
    }
    if (logIt)
        DH_LOG_I(
            "world gen features batch (%d,%d) %dx%d: %lld ms total (base %lld ms summed over tasks), structures %d of "
            "%zu cells (%d without base, %d threw%s) in %lld ms, decorated %d of %zu cells (%d without base, %d threw) "
            "in %lld ms, delivered %d, placedBlocks=%lld (structures %lld)",
            x0,
            z0,
            w,
            w,
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - batch->t0)
                    .count()
            ),
            batch->baseMsSum.load(),
            structured,
            plan.structures.size(),
            structMissing,
            structThrew,
            structuresDisabled_ ? ", DISABLED" : (noRegistry ? ", no structure registry" : ""),
            structMs,
            decorated,
            plan.decorate.size(),
            missing,
            threw,
            decoMs,
            okCount,
            placedT,
            structPlacedT
        );
}

bool BdsWorldGenerator::compareModeEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("DH_BEDROCK_GEN_COMPARE");
        return e != nullptr && std::string(e) == "1";
    }();
    return on;
}

void BdsWorldGenerator::compareWithReal(std::shared_ptr<dh::IChunk> real) {
    if (!real || !compareModeEnabled() || closing_->load()) return;
    if (compareInFlight_.fetch_add(1) >= 2) {
        compareInFlight_.fetch_sub(1);
        compareSkipped_.fetch_add(1);
        return;
    }
    const std::int32_t cx = real->chunkX(), cz = real->chunkZ();
    queueBatch(
        cx,
        cz,
        1,
        GenerationMode::Features,
        nullptr,
        [this](bool) { compareInFlight_.fetch_sub(1); },
        [this, real](
            std::shared_ptr<dh::IChunk> base,
            std::shared_ptr<dh::IChunk> afterStructures,
            std::shared_ptr<dh::IChunk> gen
        ) {
            if (!gen) return;
            const dh::ChunkDiff d = base && afterStructures
                                      ? dh::compareChunksStaged(*real, *base, *afterStructures, *gen)
                                      : dh::compareChunks(*real, *gen);
            std::lock_guard lock(compareMutex_);
            compareTotal_.add(d);
            ++compareCount_;
            DH_LOG_I("gen compare (%d,%d): %s", real->chunkX(), real->chunkZ(), d.summary().c_str());
            if (compareCount_ % 10 == 0)
                DH_LOG_I(
                    "gen compare TOTAL over %lld chunks (%lld skipped): %s",
                    compareCount_,
                    compareSkipped_.load(),
                    compareTotal_.summary().c_str()
                );
        }
    );
}

std::shared_ptr<dh::IChunk> BdsWorldGenerator::snapshotLoaded(
    const std::shared_ptr<Batch>& batch,
    std::int32_t                  cx,
    std::int32_t                  cz,
    bool                          loadIfMissing
) {
    Dimension* dim = &dimension_;
    auto       fn  = [dim, cx, cz, loadIfMissing]() -> std::shared_ptr<LevelChunk> {
        ChunkSource&     cs = dim->getChunkSource();
        const ::ChunkPos cp(cx, cz);
        if (auto lc = cs.getAvailableChunk(cp)) return lc;
        if (!loadIfMissing) return nullptr;
        return cs.getOrLoadChunk(cp, ChunkSource::LoadMode::None, /*readOnly=*/false);
    };
    std::shared_ptr<LevelChunk> lc = onServerThread(fn, 2000ms, closing_);
    if (!lc) return nullptr;

    // Both paths require a fully Loaded chunk (a chunk still Generating/Generated is incomplete). Only the
    // load path waits for it; PreExistingOnly just skips such chunks.
    const auto deadline = std::chrono::steady_clock::now() + (loadIfMissing ? kInternalServerTtl : 0ms);
    while (lc->mLoadState->load() < ChunkState::Loaded) {
        if (!loadIfMissing || closing_->load() || batch->cancelled.load()
            || std::chrono::steady_clock::now() > deadline)
            return nullptr;
        std::this_thread::sleep_for(50ms);
    }
    if (!BedrockChunk::heightSupported(*lc)) return nullptr;

    // Snapshot on the server thread (the chunk is live), then light on this worker.
    BlockSerializer* ser     = &serializer_;
    auto             wrapped = onServerThread(
        [lc, ser]() -> std::shared_ptr<ScratchChunk> {
            auto w = std::make_shared<ScratchChunk>(lc, *ser);
            (void)w->freeze();
            return w;
        },
        5000ms,
        closing_
    );
    if (!wrapped) return nullptr;
    lightBlockOnly(*wrapped);
    return wrapped;
}

} // namespace dh::bedrock
