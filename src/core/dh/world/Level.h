#pragma once

// Port of level/{AbstractDhLevel,AbstractDhServerLevel,ClientLevelModule,ServerLevelModule,DhClientLevel,
// DhClientServerLevel,DhServerLevel}.java: the per-dimension objects that compose provider, propagator, quad tree,
// request modules, network state and renderer handles.
//
//   AbstractLevel          owns ILodStore (via factory), GeneratedProvider (every level kind uses the generated
//                          provider, Java RemoteFullDataSourceProvider extends it), UpdatePropagator, the
//                          DelayedSaveCache of merged chunk updates and the chunk-hash bookkeeping.
//   ClientLevelModule      LodQuadTree + RenderList + the IVertexSink, ticked by clientTick(BlockPos2D).
//   ServerLevelModule      world gen (LodRequestModule + GenQueue + WorldGenerator over the platform's
//                          IChunkGenerator) and the ServerRequestHandler registered at the ServerNetwork.
//   DhClientLevel          client module + ClientRequestQueue (as the provider's retrieval queue) +
//                          SyncOnLoadRequestQueue (remote server).
//   DhClientServerLevel    client module + server module sharing ONE provider; retrieval queue = GenQueue.
//   DhServerLevel          server module only.
//
// Ownership / destruction (members are destroyed bottom-up; close() performs the real shutdown and is idempotent):
//
//   AbstractLevel
//     store_ ............. ILodStore               (destroyed last)
//     hashStore_/tracker_  chunk-hash table
//     provider_ .......... GeneratedProvider       references store_
//     propagator_ ........ UpdatePropagator        references provider_
//     saveCache_ ......... DelayedSaveCache        removal handler references provider_/tracker_
//   DhClientServerLevel : AbstractLevel
//     server_ ............ ServerLevelModule       chunk generator, WorldGenerator, LodRequestModule(GenQueue)
//     client_ ............ ClientLevelModule       LodQuadTree(s), RenderList
//
// close() order (PORTING.md "destruction order module -> queue -> provider"):
//   1. closeImpl(): request module(s) stop their retrieval queue (module.close() -> queue.shutdown(true), futures
//      cancelled), network handlers/listeners are detached, then the tree is closed (G2: queues first, then the
//      tree) which disposes every section (sink.remove).
//   2. the save cache is flushed SYNCHRONOUSLY (updater.update, not updateAsync, so nothing is left queued) and its
//      chunk hashes are committed per the recipe in ChunkHashStore.h.
//   3. deps.drainExecutors() (platform: join/drain the pools, tests: ManualExecutor::runAll), then
//      updater.waitQuiescent(): no queued task still references the level (provider.h lifetime rule).
//   4. provider.close(), store.flush(); members are destroyed afterwards.
//
// Threading: no threads are created. tick() is driven by the platform (see WorldDeps.h). tick() and close() of a
// level are serialised by one mutex; everything else is thread safe by the composed components.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/gen/GenQueue.h"
#include "dh/gen/GeneratedProvider.h"
#include "dh/gen/LodRequestModule.h"
#include "dh/gen/WorldGenerator.h"
#include "dh/multiplayer/ClientNetworkState.h"
#include "dh/multiplayer/ClientRequestQueue.h"
#include "dh/multiplayer/ServerRequestHandler.h"
#include "dh/platform/IChunk.h"
#include "dh/pos/ChunkPos.h"
#include "dh/render/RenderList.h"
#include "dh/storage/ChunkHashStore.h"
#include "dh/storage/UpdatePropagator.h"
#include "dh/thread/DelayedSaveCache.h"
#include "dh/tree/LodQuadTree.h"
#include "dh/tree/LodRenderSection.h"
#include "dh/world/LevelHandle.h"
#include "dh/world/WorldDeps.h"

namespace dh {

/// Cadences of the time-driven work inside AbstractLevel::tick() (Java thread loops / timers).
inline constexpr std::int64_t kClientTickMs        = 100;  // IDhClientWorld.TICK_RATE_IN_MS
inline constexpr std::int64_t kPropagatorTickMs    = 250;  // PROPAGATE_QUEUE_THREAD_DELAY_IN_MS
inline constexpr std::int64_t kSaveCacheCleanupMs  = 1000; // AbstractDelayedSaveCache static loop
inline constexpr int          kSaveCacheDelayMs    = 1000; // new DelayedDataSourceSaveCache(.., 1_000)
inline constexpr std::int64_t kSyncVisitedExpiryMs = 20 * 60 * 1000;
inline constexpr std::int64_t kLoadedOnceExpiryMs  = 10 * 60 * 1000;

//=============================//
// retrieval queue adaptation  //
//=============================//

/// What LodQuadTree submits to: Java's tree calls fullDataSourceProvider.queuePositionForRetrieval(pos), which
/// de-duplicates and applies the result (GeneratedProvider::queueRetrieval -> onRetrievalResult). The G2 tree only
/// speaks IRetrievalQueue, so this adapter routes submit() through the provider. The REAL queue (GenQueue /
/// ClientRequestQueue) is owned by the LodRequestModule and registered at the provider while world gen runs;
/// without one submit() returns nullptr, which the tree reads as "cannot generate" (Java null).
class ProviderRetrievalQueue final : public IRetrievalQueue {
public:
    explicit ProviderRetrievalQueue(GeneratedProvider& provider) : provider_(provider) {}

    std::shared_ptr<RetrievalFuture> submit(std::uint64_t sectionPos, std::uint8_t) override {
        return provider_.queueRetrieval(sectionPos);
    }
    void startAndSetTargetPos(BlockPos2D) override {} // LodRequestModule::tick retargets the real queue
    void removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) override {
        provider_.removeRetrievalRequestIf(std::move(removeIf));
    }
    std::size_t waitingTaskCount() const override { return provider_.queuedRetrievalCount(); }
    std::size_t inProgressTaskCount() const override { return 0; }
    void        shutdown(bool) override {}
    void        setCanRegenerate(bool v) override { provider_.setCanRegenerate(v); }
    bool        getCanRegenerate() const override { return provider_.canRegenerate(); }

private:
    GeneratedProvider& provider_;
};

//=====================//
// delayed chunk saves //
//=====================//

/// One chunk's contribution to a pending section save.
struct ChunkSaveInput {
    const FullDataSource* source = nullptr;
    ChunkPos              chunk;
    std::int32_t          hash = 0;
};

/// Java DelayedDataSourceSaveCache.DataSourceSaveObjContainer: the merged section plus (deviation) the chunk hashes
/// that went into THIS container. Java keyed the pending hashes by section pos only, so a hash recorded after the
/// container left the cache could be committed with the earlier save; here the hashes travel with the container and
/// are noted in the ChunkHashTracker under a unique per-container token at hand-over.
class DataSourceSaveContainer final : public SaveObjContainer<ChunkSaveInput> {
public:
    DataSourceSaveContainer(const IClock& clock, std::uint64_t pos, std::uint64_t token, const UpdateOptions& opts)
    : SaveObjContainer<ChunkSaveInput>(clock),
      merged(FullDataSource::createEmpty(pos)),
      token(token),
      options_(opts) {}

    /// Runs inside DelayedSaveCache's per-position lock (PORTING.md: mutate inside the first lock).
    void update(const ChunkSaveInput* in) override;

    FullDataSource                                 merged;
    const std::uint64_t                            token;
    std::vector<std::pair<ChunkPos, std::int32_t>> chunks; ///< latest hash per chunk, insertion order

private:
    UpdateOptions options_;
};

//===============//
// AbstractLevel //
//===============//

class AbstractLevel : public IWorldGenCompleteListener {
public:
    ~AbstractLevel() override;
    AbstractLevel(const AbstractLevel&)            = delete;
    AbstractLevel& operator=(const AbstractLevel&) = delete;

    /// false if construction failed (no store, bad deps, generator factory failed ...): the world reports it once
    /// to the user and returns nullptr. A failed level must only be destroyed.
    bool               ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    ILevelHandle&      handle() const { return handle_; }
    const WorldDeps&   deps() const { return deps_; }
    GeneratedProvider& provider() { return *provider_; }
    UpdatePropagator&  propagator() { return *propagator_; }
    ILodStore&         store() { return *store_; }
    std::string        identifier() const { return std::string(handle_.dhIdentifier()); }
    /// Java getMinY()
    std::int32_t getMinY() const { return handle_.info().minHeight(); }

    virtual bool isClientLevel() const { return false; }
    virtual bool isServerLevel() const { return false; }

    //--------------------//
    // chunk entry points //
    //--------------------//

    /// Java ChunkUpdateQueueManager.processQueuedChunkPreUpdate + processChunkUpdate +
    /// AbstractDhLevel.updateChunkAsync, run on the lod-builder executor:
    ///   1. hash skip: unchanged chunk (hash == stored hash) is dropped (:312-322; disableUnchangedChunkCheck turns it
    ///   off)
    ///   2. block light for the 3x3 neighbourhood iff !chunk.isDhBlockLightingCorrect() (sky light is baked per
    ///      section by the updater at detail 6)
    ///   3. createFromChunk -> merged into the delayed save cache (hash noted in the container)
    ///   4. when the cache hands the section over: provider.updateAsync -> commit the hashes (ChunkHashStore.h recipe)
    /// `chunk` and `neighbours` are kept alive by the queued task (PORTING.md shared_ptr<IChunk> ownership); the
    /// caller must not mutate them afterwards. Returns false if the task could not be queued.
    bool updateChunkAsync(std::shared_ptr<IChunk> chunk, std::vector<std::shared_ptr<IChunk>> neighbours = {});
    bool onChunkLoaded(std::shared_ptr<IChunk> chunk, std::vector<std::shared_ptr<IChunk>> neighbours = {}) {
        return updateChunkAsync(std::move(chunk), std::move(neighbours));
    }
    bool onChunkModified(std::shared_ptr<IChunk> chunk, std::vector<std::shared_ptr<IChunk>> neighbours = {}) {
        return updateChunkAsync(std::move(chunk), std::move(neighbours));
    }

    /// Java getChunkHash (0 = unknown)
    std::int32_t getChunkHash(ChunkPos pos) { return hashStore_->get(pos); }

    //-----------//
    // lifecycle //
    //-----------//

    /// Advances all time-driven work; cheap when nothing is due. No-op after close().
    void tick();
    /// Idempotent; see the shutdown order in the file comment.
    void close();
    bool isClosed() const { return closed_.load(); }

    /// Re-read Options (generator plan, thread counts, view distance ...). Safe from any thread.
    void onOptionsChanged();

    /// Everything this level queued has finished (tests / shutdown assertions).
    bool        isQuiescent() const;
    std::size_t unsavedChunkSections() const { return saveCache_->unsavedCount(); }
    /// Chunk updates dropped by the hash skip (observability).
    std::uint64_t skippedUnchangedChunks() const { return skipped_.load(); }
    std::uint64_t acceptedChunkUpdates() const { return accepted_.load(); }

    // IWorldGenCompleteListener (defaults: no generation)
    bool                      shouldDoWorldGen() override { return false; }
    std::optional<BlockPos2D> targetPosForGeneration() override { return std::nullopt; }
    void                      onWorldGenTaskComplete(std::uint64_t) override {}

protected:
    AbstractLevel(ILevelHandle& handle, const WorldDeps& deps);

    /// Time-driven work of the concrete level (called under the lifecycle lock, `nowMs` = steady clock).
    virtual void tickImpl(std::int64_t nowMs) { (void)nowMs; }
    /// Stop everything that produces work or calls into the provider (step 1 of the shutdown order).
    virtual void closeImpl() {}
    virtual void optionsChangedImpl() {}
    /// The block position generation / propagation is centred on.
    virtual BlockPos2D focusPos() const { return BlockPos2D{0, 0}; }
    /// Client-only levels skip chunks the server already sends (Java shouldProcessChunkUpdate).
    virtual bool shouldProcessChunkUpdate(ChunkPos) { return true; }

    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

    /// GeneratedProviderOptions / ProviderOptions / codec derived from Options + level facts.
    GeneratedProviderOptions generatedOptions() const;

    const WorldDeps& deps_;
    ILevelHandle&    handle_;

private:
    void onSaveHandover(std::shared_ptr<DataSourceSaveContainer> container);
    void finishSave(std::uint64_t sectionPos, std::uint64_t token, const Result<bool>& result);
    void runChunkUpdate(const std::shared_ptr<IChunk>& chunk, const std::vector<std::shared_ptr<IChunk>>& neighbours);

    std::string error_;

    std::unique_ptr<ILodStore>         store_;
    std::unique_ptr<IChunkHashStore>   hashStore_;
    std::unique_ptr<ChunkHashTracker>  tracker_;
    std::unique_ptr<GeneratedProvider> provider_;
    std::unique_ptr<UpdatePropagator>  propagator_;
    using SaveCache = DelayedSaveCache<ChunkSaveInput, DataSourceSaveContainer>;
    std::unique_ptr<SaveCache> saveCache_;

    std::mutex                 lifecycleMutex_;
    std::atomic<bool>          closed_{false};
    std::atomic<bool>          stopping_{false}; ///< close() started: no new chunk updates / ticks
    std::atomic<bool>          closing_{false};  ///< save hand-over uses the synchronous updater (shutdown flush)
    std::int64_t               lastPropagateMs_ = -1'000'000;
    std::int64_t               lastCleanupMs_   = -1'000'000;
    std::atomic<std::uint64_t> nextToken_{1};
    std::atomic<std::uint64_t> skipped_{0}, accepted_{0};
    std::atomic<std::int64_t>  inFlightChunkTasks_{0};
    std::atomic<std::int64_t>  outstandingSaves_{0}; ///< hand-overs whose finishSave has not run yet
    /// Chunk tasks capture this (not the level) to be retired safely, see close().
    struct TaskGate {
        std::mutex mutex;
        bool       dead    = false;
        int        running = 0;
    };
    std::shared_ptr<TaskGate> gate_   = std::make_shared<TaskGate>();
    bool                      leaked_ = false;
};

//==================//
// ClientLevelModule //
//==================//

class ClientLevelModule {
public:
    /// `source` is what render sections read (ProviderSectionSource, or the remote variant that also triggers
    /// sync-on-load requests); it must outlive the module.
    ClientLevelModule(
        const WorldDeps&          deps,
        GeneratedProvider&        provider,
        tree::ISectionDataSource& source,
        bool                      pruneRetrievalQueue
    );
    ~ClientLevelModule();
    ClientLevelModule(const ClientLevelModule&)            = delete;
    ClientLevelModule& operator=(const ClientLevelModule&) = delete;

    /// Java startRenderer: builds tree + render list state. false (and nothing changes) if already rendering.
    bool startRenderer(IVertexSink& sink, IClientLevel& clientLevel);
    /// Java stopRenderer: closes the tree (disposes sections -> sink.remove). Warns when not started.
    void stopRenderer();
    bool isRendering() const;

    /// Java clientTick: re-creates the tree if the render distance changed, then tree.tryTick(playerPos).
    void clientTick(BlockPos2D playerPos);

    /// Java reloadPos / clearRenderCache / onGeneratorPlanChanged
    void reloadPos(sec::Pos pos);
    void clearRenderCache(bool clearExistingData);
    void onGeneratorPlanChanged(bool generationEnabled);

    /// Java RenderBufferHandler.buildRenderList for this frame; the returned entries stay valid until the next call.
    const std::vector<RenderListEntry>& collectRenderList(const RenderListParams& params);

    std::shared_ptr<tree::LodQuadTree> tree() const;
    const tree::RenderSectionContext&  context() const { return ctx_; }

    /// Java ClientLevelModule.close(): stops the renderer. Closed trees are kept alive until destruction so no
    /// section callback can outlive its tree.
    void close();

private:
    void createTree(BlockPos2D start);
    void retire(std::shared_ptr<tree::LodQuadTree> t);

    const WorldDeps&   deps_;
    GeneratedProvider& provider_;
    bool               prune_;

    // transform block sets (referenced by ctx_.transform)
    BlockSet                   ignoredBlocks_, ignoredCaveBlocks_, waterSubSurface_, waterSurface_;
    ProviderRetrievalQueue     retrievalAdapter_;
    tree::RenderSectionContext ctx_;

    mutable std::mutex                              mutex_;
    std::shared_ptr<tree::LodQuadTree>              tree_;
    std::vector<std::shared_ptr<tree::LodQuadTree>> retired_;
    BlockPos2D                                      lastPlayerPos_{0, 0};
    RenderList                                      renderList_;
    std::mutex                                      renderMutex_;
};

//==================//
// ServerLevelModule //
//==================//

class ServerLevelModule {
public:
    /// `listener` is the owning level (shouldDoWorldGen / target / completion). On failure `ok()` is false.
    ServerLevelModule(
        const WorldDeps&           deps,
        ILevelHandle&              serverHandle,
        GeneratedProvider&         provider,
        IWorldGenCompleteListener& listener
    );
    ~ServerLevelModule();
    ServerLevelModule(const ServerLevelModule&)            = delete;
    ServerLevelModule& operator=(const ServerLevelModule&) = delete;

    bool               ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    /// One LodRequestModule iteration (start/stop of world gen, retarget the queue).
    void tick();
    void onOptionsChanged();

    LodRequestModule&         requestModule() { return *requestModule_; }
    mp::ServerRequestHandler* requestHandler() { return handler_.get(); }
    /// A platform chunk generator is available (otherwise this level never generates).
    bool canGenerate() const { return worldGen_ != nullptr; }
    /// The running GenQueue, or nullptr while world gen is stopped.
    GenQueue*        genQueue();
    IChunkGenerator* chunkGenerator() { return chunkGen_.get(); }

    /// Stops world gen (queue shutdown) and unregisters the request handler. Idempotent.
    void close();

private:
    const WorldDeps&   deps_;
    GeneratedProvider& provider_;
    std::string        error_;

    // order matters: destroyed bottom-up = module (queue) -> worldGen -> chunkGen
    std::unique_ptr<IChunkGenerator>          chunkGen_;
    std::unique_ptr<WorldGenerator>           worldGen_;
    std::unique_ptr<LodRequestModule>         requestModule_;
    std::unique_ptr<mp::ServerRequestHandler> handler_;
    bool                                      handlerRegistered_ = false;
};

//==========================//
// concrete levels          //
//==========================//

/// Client side of a remote-server connection (Java DhClientLevel).
class DhClientLevel final : public AbstractLevel {
public:
    /// `net` may be null (no networking: the level runs on local data only).
    DhClientLevel(ILevelHandle& clientHandle, const WorldDeps& deps, mp::ClientNetworkState* net);
    ~DhClientLevel() override;

    bool isClientLevel() const override { return true; }

    ClientLevelModule& clientside() { return *client_; }
    bool               isRendering() const { return client_->isRendering(); }
    bool               startRenderer();
    void               stopRenderer() { client_->stopRenderer(); }
    ILevelHandle&      clientHandle() const { return handle_; }
    LodRequestModule&  requestModule() { return *requestModule_; }

    /// Java clientTick: only the level the player is in ticks its tree; also ticks the sync-on-load queue.
    void clientTick();
    /// Java shouldProcessChunkUpdate (public: platform asks before spending work on a chunk).
    bool shouldProcessChunk(ChunkPos pos) { return shouldProcessChunkUpdate(pos); }

    bool                      shouldDoWorldGen() override;
    std::optional<BlockPos2D> targetPosForGeneration() override;
    void                      onWorldGenTaskComplete(std::uint64_t pos) override;

protected:
    void       tickImpl(std::int64_t nowMs) override;
    void       closeImpl() override;
    void       optionsChangedImpl() override;
    BlockPos2D focusPos() const override;
    bool       shouldProcessChunkUpdate(ChunkPos pos) override;

private:
    class RemoteSource;
    void applyPlan();

    mp::ClientNetworkState* net_;
    std::shared_ptr<bool>   alive_ = std::make_shared<bool>(true);

    std::unique_ptr<mp::SyncOnLoadRequestQueue> syncQueue_;
    std::unique_ptr<tree::ISectionDataSource>   source_;
    std::unique_ptr<ClientLevelModule>          client_;
    std::unique_ptr<LodRequestModule>           requestModule_;
    std::uint64_t                               configListener_   = 0;
    std::int64_t                                lastClientTickMs_ = -1'000'000;
    std::int64_t                                lastExpiryMs_     = 0;
    std::atomic<std::int32_t>                   lastPlayerX_{0}, lastPlayerZ_{0};

    std::mutex                                                    loadedOnceMutex_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::int64_t> loadedOnce_; ///< chunk -> expiry ms
};

/// Singleplayer: in-process server + client sharing ONE provider (Java DhClientServerLevel).
class DhClientServerLevel final : public AbstractLevel {
public:
    DhClientServerLevel(ILevelHandle& serverHandle, const WorldDeps& deps);
    ~DhClientServerLevel() override;

    bool isClientLevel() const override { return true; }
    bool isServerLevel() const override { return true; }

    ClientLevelModule& clientside() { return *client_; }
    ServerLevelModule& serverside() { return *server_; }

    /// Registers `clientHandle` as a viewer and starts rendering the dimension (sink from deps.sinkFactory) if it
    /// is not rendering yet. false if it cannot render.
    bool startRenderer(ILevelHandle& clientHandle);
    /// Forgets a viewer (Java: client wrapper unloaded). The renderer keeps running for the others.
    void        forgetClientHandle(ILevelHandle& clientHandle);
    std::size_t clientHandleCount() const;
    void        stopRenderer();
    bool        isRendering() const { return client_->isRendering(); }

    void clientTick();

    bool                      shouldDoWorldGen() override;
    std::optional<BlockPos2D> targetPosForGeneration() override;
    void                      onWorldGenTaskComplete(std::uint64_t pos) override;

protected:
    void       tickImpl(std::int64_t nowMs) override;
    void       closeImpl() override;
    void       optionsChangedImpl() override;
    BlockPos2D focusPos() const override;

private:
    std::unique_ptr<tree::ProviderSectionSource> source_;
    std::unique_ptr<ClientLevelModule>           client_;
    std::unique_ptr<ServerLevelModule>           server_;
    mutable std::mutex                           handlesMutex_;
    std::vector<ILevelHandle*>                   clientHandles_;
    std::int64_t                                 lastClientTickMs_ = -1'000'000;
    std::atomic<std::int32_t>                    lastPlayerX_{0}, lastPlayerZ_{0};
    std::size_t                                  playerCursor_ = 0;
};

/// Dedicated server level (Java DhServerLevel).
class DhServerLevel final : public AbstractLevel {
public:
    DhServerLevel(ILevelHandle& serverHandle, const WorldDeps& deps);
    ~DhServerLevel() override;

    bool isServerLevel() const override { return true; }

    ServerLevelModule& serverside() { return *server_; }

    bool                      shouldDoWorldGen() override;
    std::optional<BlockPos2D> targetPosForGeneration() override;
    void                      onWorldGenTaskComplete(std::uint64_t pos) override;

protected:
    void       tickImpl(std::int64_t nowMs) override;
    void       closeImpl() override;
    void       optionsChangedImpl() override;
    BlockPos2D focusPos() const override;

private:
    std::unique_ptr<ServerLevelModule> server_;
    std::size_t                        playerCursor_ = 0;
};

using Level = AbstractLevel;

/// Java AbstractDhServerLevel.getTargetPosForGeneration: fair round-robin over the players currently in `levelName`
/// (every call moves to the next player); (0,0) when nobody is there (Java returned ZERO).
BlockPos2D nextPlayerTarget(mp::ServerPlayers* players, const std::string& levelName, std::size_t& cursor);

} // namespace dh
