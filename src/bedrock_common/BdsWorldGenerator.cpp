#include "bedrock_common/BdsWorldGenerator.h"

#include "bedrock_common/FileLogger.h"

#include "dh/build/LightingEngine.h"

#include "mc/util/Random.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkBlockPos.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/levelgen/WorldGenerator.h"

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
class ScratchRegionSource final : public ::ChunkSource {
public:
    ScratchRegionSource(::Dimension& dim, ::Level& level) : ::ChunkSource(nullptr) {
        mLevel     = &level;
        mDimension = &dim;
    }
    void add(const std::shared_ptr<LevelChunk>& lc, std::int32_t cx, std::int32_t cz) {
        if (lc) chunks_[key(cx, cz)] = lc;
    }
    std::shared_ptr<LevelChunk> getExistingChunk(::ChunkPos const& pos) override { return find(pos); }
    std::shared_ptr<LevelChunk> getOrLoadChunk(::ChunkPos const& pos, LoadMode, bool) override { return find(pos); }
    std::shared_ptr<LevelChunk> createNewChunk(::ChunkPos const&, LoadMode, bool) override { return nullptr; }
    std::shared_ptr<LevelChunk> getRandomChunk(::Random&) override { return nullptr; }
    bool                        isChunkKnown(::ChunkPos const& pos) override { return find(pos) != nullptr; }
    bool                        isWithinWorldLimit(::ChunkPos const&) const override { return true; }

private:
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
        bool                        done = false;
        std::shared_ptr<LevelChunk> lc;
    };
    std::mutex                                                             cellsMutex;
    std::map<std::pair<std::int32_t, std::int32_t>, std::shared_ptr<Cell>> cells;
    std::mutex decoMutex; // decoration + snapshot are serialised per batch
    ~Batch() {
        // The scratch LevelChunks were constructed on the server thread: destroy them there.
        std::vector<std::shared_ptr<LevelChunk>> lcs;
        for (auto& kv : cells)
            if (kv.second && kv.second->lc) lcs.push_back(std::move(kv.second->lc));
        if (lcs.empty()) return;
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
    ChunkCallback     onChunk;
    DoneCallback      done;
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
    featuresDisabled_ = std::getenv("DH_BEDROCK_NO_FEATURES") != nullptr;
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
    if (widthChunks <= 0 || closing_->load()) {
        if (done) done(false);
        return;
    }
    if (mode == GenerationMode::Features && !warnedFeatures_.exchange(true)) {
        if (featuresDisabled_)
            DH_LOG_W("world gen: Features mode disabled by DH_BEDROCK_NO_FEATURES, generating Surface (base terrain)");
        else
            DH_LOG_I(
                "world gen: Features mode: base terrain of the 3x3 scratch neighbourhood + "
                "WorldGenerator::decorateWorldGenPostProcess on the centre (no structures); slower than Surface"
            );
    }

    auto batch       = std::make_shared<Batch>();
    batch->mode      = mode;
    batch->onChunk   = std::move(onChunk);
    batch->done      = std::move(done);
    batch->remaining = widthChunks * widthChunks;
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
            startWorkersLocked();
            work_.emplace_back([this, batch, cx, cz, guard] {
                guard->ran = true;
                runChunk(batch, cx, cz);
            });
        }
    }
    workCv_.notify_all();
}

void BdsWorldGenerator::noteChunkDone(long long ms, bool okChunk, long long featMs) {
    (okChunk ? statChunks_ : statFailed_).fetch_add(1);
    statMs_.fetch_add(ms);
    if (featMs >= 0) {
        statFeatChunks_.fetch_add(1);
        statFeatMs_.fetch_add(featMs);
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
            fn > 0 ? (std::string(", features avg ") + std::to_string(fms / fn) + " ms").c_str() : ""
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
    long long  featMs = -1;
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
                chunk = generateFeatures(batch, cx, cz, featMs);
                ok    = chunk != nullptr;
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
        if (chunk && !batch->cancelled.load() && batch->onChunk) {
            ++generated_;
            noteChunkDone(taskMs, true, featMs);
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

std::shared_ptr<dh::IChunk> BdsWorldGenerator::generateFeatures(
    const std::shared_ptr<Batch>& batch,
    std::int32_t                  cx,
    std::int32_t                  cz,
    long long&                    featMs
) {
    static std::atomic<int> taskSeq{0};
    const int               seq     = taskSeq.fetch_add(1);
    const bool              logIt   = seq < 10;
    const auto              t0      = std::chrono::steady_clock::now();
    auto                    msSince = [](std::chrono::steady_clock::time_point t) {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count()
        );
    };

    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    if (gen == nullptr) {
        DH_LOG_E("world gen: dimension has no WorldGenerator");
        return nullptr;
    }
    ::Dimension* dim = &dimension_;

    // Base terrain of one scratch chunk (construct on the server thread, loadChunk on this worker). The LevelChunk
    // stays alive in the batch cache; ~Batch destroys it on the server thread.
    auto baseChunk = [&](std::int32_t x, std::int32_t z) -> std::shared_ptr<LevelChunk> {
        auto            cell = batch->cell(x, z);
        std::lock_guard lock(cell->m);
        if (cell->done) return cell->lc;
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
        if (!lc) return nullptr;
        if (!BedrockChunk::heightSupported(*lc)) {
            cell->lc = std::move(lc); // keep it so ~Batch destroys it on the server thread
            return nullptr;
        }
        (void)lc->tryChangeState(ChunkState::Unloaded, ChunkState::Generating);
        gen->loadChunk(*lc, /*forceImmediateReplacementDataLoad=*/false);
        (void)lc->tryChangeState(ChunkState::Generating, ChunkState::Generated);
        cell->lc = std::move(lc);
        return cell->lc;
    };

    auto centre = baseChunk(cx, cz);
    if (!centre) {
        if (logIt) DH_LOG_W("world gen task %d (%d,%d): features FAILED, no base chunk", seq, cx, cz);
        return nullptr;
    }
    std::shared_ptr<LevelChunk> ring[3][3];
    ring[1][1] = centre;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx != 0 || dz != 0) {
                if (batch->cancelled.load() || closing_->load()) return nullptr;
                ring[dz + 1][dx + 1] = baseChunk(cx + dx, cz + dz);
            }
    const long long baseMs = msSince(t0);

    // Decoration + snapshot, serialised per batch: the ring chunks are shared between the batch's tasks and the
    // features write into them.
    std::lock_guard deco(batch->decoMutex);
    if (batch->cancelled.load() || closing_->load()) return nullptr;
    const auto f0         = std::chrono::steady_clock::now();
    bool       decorated  = false;
    int        sourceHits = 0;
    try {
        ::Level&            level = static_cast<::Level&>(dimension_.mLevel);
        ScratchRegionSource src(dimension_, level);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (ring[dz + 1][dx + 1]) {
                    src.add(ring[dz + 1][dx + 1], cx + dx, cz + dz);
                    ++sourceHits;
                }
        ::BlockSource bs(
            level,
            dimension_,
            src,
            /*publicSource=*/false,
            /*allowUnpopulatedChunks=*/true,
            /*allowClientTickingChanges=*/false,
            /*markChangedBlocksForSaving=*/false
        );
        // Per-chunk decoration seed (the classic "populate" scheme): chunk position mixed with two odd
        // multipliers drawn from the level seed.
        const unsigned seed32 = static_cast<unsigned>(level.getSeed());
        ::Random       seedRng(seed32, true);
        const int64_t  xs = seedRng.nextLong() / 2 * 2 + 1;
        const int64_t  zs = seedRng.nextLong() / 2 * 2 + 1;
        const unsigned ds = static_cast<unsigned>(
            (static_cast<int64_t>(cx) * xs + static_cast<int64_t>(cz) * zs) ^ static_cast<int64_t>(seed32)
        );
        ::Random       rnd(ds, true);
        const int      minY  = dimension_.mHeightRange->mMin;
        const short    seaY  = static_cast<short>(std::clamp(63 - minY, 0, dimension_.mHeightRange->mMax - minY - 1));
        const ::Biome& biome = centre->getBiome(::ChunkBlockPos{8, ChunkLocalHeight{seaY}, 8});
        gen->decorateWorldGenPostProcess(biome, *centre, bs, rnd);
        decorated = true;
    } catch (...) {
        DH_LOG_W("world gen task %d (%d,%d): decoration threw, delivering base terrain only", seq, cx, cz);
    }

    auto wrapped = std::make_shared<ScratchChunk>(centre, serializer_);
    (void)wrapped->freeze(); // the cache keeps the LevelChunk alive
    featMs = msSince(f0);

    long long nonAir = 0;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (std::int32_t y = wrapped->minNonEmptyHeight(); y < wrapped->maxNonEmptyHeight(); ++y)
                if (!wrapped->blockAt(x, y, z)->isAir) ++nonAir;
    if (logIt)
        DH_LOG_I(
            "world gen task %d (%d,%d): %lld ms (base %lld ms), features=%d (%lld ms, ring chunks %d/9), nonAir=%lld",
            seq,
            cx,
            cz,
            msSince(t0),
            baseMs,
            decorated ? 1 : 0,
            featMs,
            sourceHits,
            nonAir
        );
    if (nonAir == 0) return nullptr;
    lightBlockOnly(*wrapped);
    return wrapped;
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
