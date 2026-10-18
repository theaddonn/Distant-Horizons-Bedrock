#include "server/BdsWorldGenerator.h"

#include "bedrock_common/FileLogger.h"

#include "dh/build/LightingEngine.h"

#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/levelgen/WorldGenerator.h"

#include "ll/api/thread/ServerThreadExecutor.h"

#include <chrono>
#include <future>
#include <thread>

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

    /// snapshot() + free the LevelChunk: afterwards this wrapper never dereferences it.
    void freeze() {
        snapshot();
        lc.reset();
    }
};

/// Runs `fn` on the server thread and waits up to `ttl` for its result (nullptr on timeout).
template <class Fn>
auto onServerThread(Fn fn, std::chrono::milliseconds ttl) -> decltype(fn()) {
    using R   = decltype(fn());
    auto prom = std::make_shared<std::promise<R>>();
    auto fut  = prom->get_future();
    ll::thread::ServerThreadExecutor::getDefault().execute([prom, fn = std::move(fn)]() mutable {
        try {
            prom->set_value(fn());
        } catch (...) {
            try {
                prom->set_value(R{});
            } catch (...) {}
        }
    });
    if (fut.wait_for(ttl) != std::future_status::ready) return R{};
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
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ok{true};
    std::atomic<int>  remaining{0};
    GenerationMode    mode = GenerationMode::Surface;
    ChunkCallback     onChunk;
    DoneCallback      done;
};

BdsWorldGenerator::BdsWorldGenerator(Dimension& dimension, BlockSerializer& serializer, dh::IExecutor& genPool)
: dimension_(dimension),
  serializer_(serializer),
  genPool_(genPool) {}

void BdsWorldGenerator::close() {
    closing_.store(true);
    cancelAll();
}

/// Owned by the pool task's closure: whether the task runs or is dropped (ThreadPools::shutdown() drops queued
/// tasks), its destruction releases the in-flight slot and, if the task never ran, completes the chunk as failed.
struct BdsWorldGenerator::TaskGuard {
    BdsWorldGenerator*     self;
    std::shared_ptr<Batch> batch;
    bool                   ran = false;
    ~TaskGuard() {
        if (!ran) self->finishChunk(batch, false);
        std::lock_guard lock(self->mutex_);
        if (--self->inFlightTasks_ == 0) self->idleCv_.notify_all();
    }
};

BdsWorldGenerator::~BdsWorldGenerator() {
    close();
    std::unique_lock lock(mutex_);
    // Tasks capture `this`: wait for them (they skip the work once cancelled). Bounded so a stuck server
    // thread hop cannot hang shutdown forever.
    idleCv_.wait_for(lock, 30s, [&] { return inFlightTasks_.load() == 0; });
}

bool BdsWorldGenerator::isBusy() const {
    // Java: the queue itself bounds in-flight tasks; also stay out of the way while the server thread is the
    // only way to progress (InternalServer waits there).
    return inFlightTasks_.load() > 64;
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
    if (widthChunks <= 0 || closing_.load()) {
        if (done) done(false);
        return;
    }
    if (mode == GenerationMode::Features && !warnedFeatures_.exchange(true))
        DH_LOG_W("world gen: Features mode is not implemented yet, generating Surface (base terrain) instead");

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
            auto guard = std::make_shared<TaskGuard>(TaskGuard{this, batch});
            // If rejected, the closure (and with it the guard) is destroyed and completes the chunk as failed.
            (void)genPool_.execute([this, batch, cx, cz, guard] {
                guard->ran = true;
                runChunk(batch, cx, cz);
            });
        }
    }
}

void BdsWorldGenerator::finishChunk(const std::shared_ptr<Batch>& batch, bool ok) {
    if (!ok) batch->ok.store(false);
    if (batch->remaining.fetch_sub(1) == 1) {
        const bool allOk = batch->ok.load() && !batch->cancelled.load();
        if (batch->done) batch->done(allOk);
    }
}

void BdsWorldGenerator::runChunk(const std::shared_ptr<Batch>& batch, std::int32_t cx, std::int32_t cz) {
    if (batch->cancelled.load() || closing_.load()) {
        finishChunk(batch, false);
        return;
    }
    bool ok = true;
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
        case GenerationMode::Surface:
        case GenerationMode::Features:
        default:
            chunk = generateScratch(cx, cz);
            ok    = chunk != nullptr;
            break;
        }
        if (chunk && !batch->cancelled.load() && batch->onChunk) {
            ++generated_;
            batch->onChunk(std::move(chunk));
        } else if (!chunk && !ok) {
            ++failed_;
        }
    } catch (...) {
        ok = false;
        ++failed_;
        DH_LOG_E("world gen: exception swallowed for chunk %d,%d", cx, cz);
    }
    finishChunk(batch, ok);
}

std::shared_ptr<dh::IChunk> BdsWorldGenerator::generateScratch(std::int32_t cx, std::int32_t cz) {
    ::WorldGenerator* gen = dimension_.mWorldGenerator;
    if (gen == nullptr) {
        DH_LOG_E("world gen: dimension has no WorldGenerator");
        return nullptr;
    }
    const ::ChunkPos cp(cx, cz);

    // 1. scratch chunk (see header). createNewChunk is the same call vanilla's getOrLoadChunk makes.
    std::shared_ptr<LevelChunk> lc = gen->createNewChunk(cp, ChunkSource::LoadMode::None, /*readOnly=*/false);
    if (!lc) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            DH_LOG_W("world gen: ChunkSource::createNewChunk returned null, using the LevelChunk constructor");
        lc = std::shared_ptr<LevelChunk>(new LevelChunk(
            dimension_,
            cp,
            /*readOnly=*/false,
            SubChunkInitMode::All,
            /*initializeMetaData=*/false,
            LevelChunkBlockActorStorage::TrackingMode::None
        ));
    }
    if (!BedrockChunk::heightSupported(*lc)) return nullptr;

    // 2.-3. generate base terrain. TODO(verify in game): whether loadChunk expects the Generating state or
    // sets it itself; tryChangeState is a no-op (returns false) when the transition does not apply.
    (void)lc->tryChangeState(ChunkState::Unloaded, ChunkState::Generating);
    gen->loadChunk(*lc, /*forceImmediateReplacementDataLoad=*/false);
    (void)lc->tryChangeState(ChunkState::Generating, ChunkState::Generated);

    // 4. snapshot, free the LevelChunk, light.
    auto wrapped = std::make_shared<ScratchChunk>(std::move(lc), serializer_);
    wrapped->freeze();
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
    std::shared_ptr<LevelChunk> lc = onServerThread(fn, 2000ms);
    if (!lc) return nullptr;

    // Both paths require a fully Loaded chunk (a chunk still Generating/Generated is incomplete). Only the
    // load path waits for it; PreExistingOnly just skips such chunks.
    const auto deadline = std::chrono::steady_clock::now() + (loadIfMissing ? kInternalServerTtl : 0ms);
    while (lc->mLoadState->load() < ChunkState::Loaded) {
        if (!loadIfMissing || closing_.load() || batch->cancelled.load() || std::chrono::steady_clock::now() > deadline)
            return nullptr;
        std::this_thread::sleep_for(50ms);
    }
    if (!BedrockChunk::heightSupported(*lc)) return nullptr;

    // Snapshot on the server thread (the chunk is live), then light on this worker.
    BlockSerializer* ser     = &serializer_;
    auto             wrapped = onServerThread(
        [lc, ser]() -> std::shared_ptr<ScratchChunk> {
            auto w = std::make_shared<ScratchChunk>(lc, *ser);
            w->freeze();
            return w;
        },
        5000ms
    );
    if (!wrapped) return nullptr;
    lightBlockOnly(*wrapped);
    return wrapped;
}

} // namespace dh::bedrock
