#include "dh/world/Level.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "dh/fulldata/Merge.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/FullToRender.h"

namespace dh {

namespace {

GenerationMode toGenerationMode(DistantGeneratorMode mode) { return static_cast<GenerationMode>(complexity(mode)); }

GeneratorPlan toConfigPlan(net::GeneratorPlan plan) {
    return static_cast<GeneratorPlan>(static_cast<std::uint8_t>(plan));
}

std::int64_t steadyMs(const IClock& clock) { return clock.steadyNanos() / 1'000'000; }

CodecOptions codecFrom(const Options&) {
    // TODO(WP-D1b): Compression::Zstd is reserved until zstd is vendored (Codec.h); DataCompressionMode::ZStdBlock
    // therefore stores uncompressed for now. The store format records the mode, so enabling it later is transparent.
    return CodecOptions{};
}

} // namespace

//===========================//
// DataSourceSaveContainer   //
//===========================//

void DataSourceSaveContainer::update(const ChunkSaveInput* in) {
    // Java DataSourceSaveObjContainer.update: this.dataSource.updateFromDataSource(newObj)
    if (in == nullptr || in->source == nullptr) return;
    (void)updateFromDataSource(merged, *in->source, options_);
    for (auto& entry : chunks) {
        if (entry.first == in->chunk) {
            entry.second = in->hash; // latest content of the chunk wins
            return;
        }
    }
    chunks.emplace_back(in->chunk, in->hash);
}

//===============//
// AbstractLevel //
//===============//

AbstractLevel::AbstractLevel(ILevelHandle& handle, const WorldDeps& deps) : deps_(deps), handle_(handle) {
    if (!deps.valid()) {
        fail(
            "incomplete WorldDeps (clock, registry, options and the file-handler / lod-builder / propagator executors "
            "are required)"
        );
        return;
    }
    if (!deps.storeFactory) {
        fail("no LOD store factory");
        return;
    }
    store_ = deps.storeFactory(handle);
    if (!store_) {
        fail("unable to open the LOD store");
        return;
    }
    hashStore_ = deps.chunkHashFactory ? deps.chunkHashFactory(handle) : nullptr;
    if (!hashStore_) hashStore_ = std::make_unique<MemoryChunkHashStore>();
    tracker_ = std::make_unique<ChunkHashTracker>(*hashStore_);

    const Options&  o = *deps.options;
    ProviderOptions popts;
    popts.update.worldCompression = o.common.lodBuilding.worldCompression;
    popts.maxSkyLight             = handle.info().hasSkyLight() ? 15 : 0;
    provider_                     = std::make_unique<GeneratedProvider>(
        *store_,
        *deps.registry,
        *deps.clock,
        *deps.exec.fileHandler,
        codecFrom(o),
        popts,
        generatedOptions()
    );
    propagator_ = std::make_unique<UpdatePropagator>(
        *provider_,
        *deps.exec.updatePropagator,
        std::max(1, o.common.multiThreading.numberOfThreads)
    );

    saveCache_ = std::make_unique<SaveCache>(
        [this](std::int64_t pos) {
            UpdateOptions uo;
            uo.worldCompression = deps_.options->common.lodBuilding.worldCompression;
            return std::make_shared<DataSourceSaveContainer>(
                *deps_.clock,
                static_cast<std::uint64_t>(pos),
                nextToken_.fetch_add(1),
                uo
            );
        },
        [this](std::shared_ptr<DataSourceSaveContainer> c) { onSaveHandover(std::move(c)); },
        kSaveCacheDelayMs
    );

    // Java WorldChunkUpdateManager.INSTANCE.getTotalQueuedCount(): chunk updates that are queued or merging.
    provider_->setQueuedChunkUpdateCounter([this] {
        return static_cast<int>(inFlightChunkTasks_.load() + static_cast<std::int64_t>(saveCache_->unsavedCount()));
    });
}

AbstractLevel::~AbstractLevel() { close(); }

GeneratedProviderOptions AbstractLevel::generatedOptions() const {
    GeneratedProviderOptions g;
    g.plan            = deps_.options->common.worldGenerator.generatorPlan;
    g.numberOfThreads = std::max(1, deps_.options->common.multiThreading.numberOfThreads);
    return g;
}

bool AbstractLevel::updateChunkAsync(std::shared_ptr<IChunk> chunk, std::vector<std::shared_ptr<IChunk>> neighbours) {
    if (!chunk || !ok() || stopping_.load() || closed_.load()) return false;
    if (!shouldProcessChunkUpdate(ChunkPos(chunk->chunkX(), chunk->chunkZ()))) return false;

    inFlightChunkTasks_.fetch_add(1);
    // PORTING.md "shared_ptr<IChunk> ownership": the task owns the chunk and its neighbours until it is done.
    const bool queued =
        deps_.exec.lodBuilder->execute([this, chunk = std::move(chunk), neighbours = std::move(neighbours)] {
            runChunkUpdate(chunk, neighbours);
            inFlightChunkTasks_.fetch_sub(1);
        });
    if (!queued) inFlightChunkTasks_.fetch_sub(1);
    return queued;
}

void AbstractLevel::runChunkUpdate(
    const std::shared_ptr<IChunk>&              chunk,
    const std::vector<std::shared_ptr<IChunk>>& neighbours
) {
    if (closed_.load()) return;
    const ChunkPos     cp(chunk->chunkX(), chunk->chunkZ());
    const std::int32_t hash = static_cast<std::int32_t>(chunk->blockBiomeHashCode());

    // 1. unchanged-chunk skip (ChunkUpdateQueueManager.java:312-322). 0 = unknown hash, like Java's getChunkHash.
    const bool checkChunkHash = !deps_.options->common.lodBuilding.disableUnchangedChunkCheck;
    if (checkChunkHash && hashStore_->get(cp) == hash) {
        skipped_.fetch_add(1);
        return;
    }

    // 2. 3x3 block lighting (DhLightingEngine.bakeChunkBlockLighting, ChunkUpdateQueueManager.java:380); sky light is
    // baked per section by the updater at detail 6. Platform drivers may already have lit the chunk.
    if (!chunk->isDhBlockLightingCorrect()) {
        std::vector<IChunk*> nearby;
        nearby.reserve(8);
        for (const auto& n : neighbours) {
            if (!n || n.get() == chunk.get()) continue;
            const std::int32_t dx = n->chunkX() - cp.x, dz = n->chunkZ() - cp.z;
            if (dx < -1 || dx > 1 || dz < -1 || dz > 1) continue;
            nearby.push_back(n.get());
        }
        LightingOptions lo;
        lo.maxSkyLight        = handle_.info().hasSkyLight() ? 15 : 0;
        lo.generateBlockLight = true;
        lo.generateSkyLight   = false;
        lightChunk(*chunk, std::span<IChunk* const>(nearby.data(), nearby.size()), lo);
    }

    // 3. chunk -> LOD section, merged in memory with the other chunks of the same section
    BuildOptions bo;
    bo.worldCompression                 = deps_.options->common.lodBuilding.worldCompression;
    std::optional<FullDataSource> built = createFromChunk(*chunk, bo);
    if (!built) return; // Java: "a chunk save is superseded by a later event"
    accepted_.fetch_add(1);
    ChunkSaveInput in;
    in.source = &*built;
    in.chunk  = cp;
    in.hash   = hash;
    saveCache_->writeToMemoryAndQueueSave(static_cast<std::int64_t>(built->pos), &in);
}

void AbstractLevel::onSaveHandover(std::shared_ptr<DataSourceSaveContainer> container) {
    const std::uint64_t pos     = container->merged.pos;
    const std::uint64_t token   = container->token;
    const bool          hashing = !deps_.options->common.lodBuilding.disableUnchangedChunkCheck;
    if (hashing) {
        for (const auto& [chunk, hash] : container->chunks) tracker_->notePending(token, chunk, hash);
    }
    auto section = std::make_shared<const FullDataSource>(std::move(container->merged));
    container.reset();

    if (closing_.load()) {
        // shutdown flush: synchronous so no task is left on an executor that is about to be drained
        finishSave(pos, token, provider_->updater().update(section));
        return;
    }
    // Java onDataSourceSaveAsync: updateDataSourcesAsync(...).thenRun(save hashes)
    std::shared_ptr<UpdateFuture> fut = provider_->updateAsync(section);
    fut->onComplete([this, pos, token](const Result<bool>& r) { finishSave(pos, token, r); });
}

void AbstractLevel::finishSave(std::uint64_t sectionPos, std::uint64_t token, const Result<bool>& result) {
    // Commit recipe (ChunkHashStore.h): commit on Ok(true); on Ok(false) only while the provider is open (updateAsync
    // also reports Ok(false) when it was rejected) and the section really exists in the store; discard on errors.
    bool commit = false;
    if (result.has_value() && !provider_->isClosed()) {
        commit = *result || provider_->timestamp(sectionPos).has_value();
    }
    if (commit) (void)tracker_->commit(token);
    else tracker_->discard(token);
}

void AbstractLevel::tick() {
    std::lock_guard lock(lifecycleMutex_);
    if (closed_.load() || stopping_.load() || !ok()) return;
    const std::int64_t now = steadyMs(*deps_.clock);
    tickImpl(now);
    if (now - lastPropagateMs_ >= kPropagatorTickMs) {
        lastPropagateMs_ = now;
        propagator_->tick(focusPos());
    }
    if (now - lastCleanupMs_ >= kSaveCacheCleanupMs) {
        lastCleanupMs_ = now;
        saveCache_->cleanUp(false); // replaces Java's static 1 s cleanup thread
    }
}

void AbstractLevel::close() {
    std::lock_guard lock(lifecycleMutex_);
    if (closed_.load()) return;
    stopping_.store(true);

    // 1. modules: world gen / request queues / network handlers / trees (module -> queue -> provider order)
    closeImpl();

    if (ok()) {
        // 2. let already queued chunk updates reach the save cache
        if (deps_.drainExecutors) deps_.drainExecutors();

        // 3. flush the delayed saves synchronously (Java just stopped the cache and lost up to 1 s of edits)
        closing_.store(true);
        saveCache_->close();
        saveCache_->flush();

        // 4. nothing may reference the level any more
        if (deps_.drainExecutors) deps_.drainExecutors();
        provider_->updater().waitQuiescent();
        provider_->close();
        (void)store_->flush();
    }
    closed_.store(true);
}

void AbstractLevel::onOptionsChanged() {
    if (!ok() || closed_.load()) return;
    const Options& o = *deps_.options;
    provider_->setGeneratorPlan(o.common.worldGenerator.generatorPlan);
    provider_->setNumberOfThreads(std::max(1, o.common.multiThreading.numberOfThreads));
    optionsChangedImpl();
}

bool AbstractLevel::isQuiescent() const {
    if (!ok()) return true;
    return saveCache_->unsavedCount() == 0 && tracker_->pendingSectionCount() == 0 && inFlightChunkTasks_.load() == 0
        && provider_->updater().pendingAsyncCount() == 0 && provider_->updater().inFlightUpdates() == 0
        && propagator_->updatingCount() == 0;
}

//==================//
// ClientLevelModule //
//==================//

ClientLevelModule::ClientLevelModule(
    const WorldDeps&          deps,
    GeneratedProvider&        provider,
    tree::ISectionDataSource& source,
    bool                      pruneRetrievalQueue
)
: deps_(deps),
  provider_(provider),
  prune_(pruneRetrievalQueue),
  retrievalAdapter_(provider) {
    const Options& o   = *deps.options;
    ignoredBlocks_     = resolveCsv(o.client.graphics.culling.ignoredRenderBlockCsv, *deps.registry, true);
    ignoredCaveBlocks_ = resolveCsv(o.client.graphics.culling.ignoredRenderCaveBlockCsv, *deps.registry, true);
    waterSubSurface_   = resolveCsv(o.client.graphics.culling.waterSubSurfaceBlockReplacementCsv, *deps.registry);
    waterSurface_      = resolveCsv(o.client.graphics.culling.waterSurfaceBlockReplacementCsv, *deps.registry);

    ctx_.data         = &source;
    ctx_.registry     = deps.registry;
    ctx_.renderLoader = deps.exec.renderLoader;

    const QualityOptions& q               = o.client.graphics.quality;
    const CullingOptions& c               = o.client.graphics.culling;
    ctx_.transform.verticalQuality        = q.verticalQuality;
    ctx_.transform.blocksToAvoid          = c.blocksToIgnore;
    ctx_.transform.tintWithAvoidedBlocks  = c.tintWithAvoidedBlocks;
    ctx_.transform.enableCaveCulling      = c.enableCaveCulling;
    ctx_.transform.caveCullingHeight      = c.caveCullingHeight;
    ctx_.transform.transparency           = q.transparency;
    ctx_.transform.ignoredBlocks          = &ignoredBlocks_;
    ctx_.transform.ignoredCaveBlocks      = &ignoredCaveBlocks_;
    ctx_.transform.waterSubSurfaceReplace = &waterSubSurface_;
    ctx_.transform.waterSurfaceReplace    = &waterSurface_;
    ctx_.mesh.quad.transparency           = q.transparency;
    ctx_.mesh.quad.grassSide              = q.grassSideRendering;
    ctx_.mesh.enableCaveCulling           = c.enableCaveCulling;
    ctx_.mesh.caveCullingHeight           = c.caveCullingHeight;
    ctx_.mesh.brightnessMultiplier        = q.brightnessMultiplier;
    ctx_.mesh.saturationMultiplier        = q.saturationMultiplier;
}

ClientLevelModule::~ClientLevelModule() { close(); }

bool ClientLevelModule::startRenderer(IVertexSink& sink, IClientLevel& clientLevel) {
    std::lock_guard lock(mutex_);
    if (tree_) {
        log::logf(
            LogLevel::Warn,
            "Renderer already started for [%.*s].",
            static_cast<int>(clientLevel.dimensionName().size()),
            clientLevel.dimensionName().data()
        );
        return false;
    }
    if (deps_.exec.renderLoader == nullptr) {
        log::logf(LogLevel::Error, "Unable to start the renderer: no render-loader executor.");
        return false;
    }
    ctx_.level                   = &clientLevel;
    ctx_.sink                    = &sink;
    ctx_.mesh.quad.dirtColorArgb = clientLevel.dirtBlockColor();
    for (int i = 0; i < 6; ++i)
        ctx_.mesh.quad.shadeByFace[static_cast<std::size_t>(i)] = clientLevel.shade(static_cast<Direction>(i));
    createTree(lastPlayerPos_);
    return true;
}

void ClientLevelModule::createTree(BlockPos2D start) {
    // mutex_ held
    tree::LodQuadTreeDeps d;
    d.retrievalQueue         = &retrievalAdapter_;
    d.retrievalQueueExecutor = deps_.exec.retrievalQueuing;
    d.fileHandler            = deps_.exec.fileHandler;
    d.positionsToRetrieve    = [this](sec::Pos pos, std::int8_t generatorDetail) {
        return provider_.positionsToRetrieve(pos, static_cast<std::uint8_t>(generatorDetail));
    };
    d.canQueueRetrievalNow = [this] { return provider_.canQueueRetrievalNow(prune_); };
    // Java ClientRenderState: initial position (0,0) "just in case the player hasn't loaded in yet"
    tree_ = std::make_shared<tree::LodQuadTree>(
        *deps_.options,
        tree::LodQuadTree::viewDiameterInBlocks(*deps_.options),
        start,
        ctx_,
        std::move(d)
    );
    tree_->attachToProvider(provider_); // Java ClientLevelModule: addDataSourceUpdateListener(this) -> queuePosToReload
}

void ClientLevelModule::stopRenderer() {
    std::shared_ptr<tree::LodQuadTree> t;
    {
        std::lock_guard lock(mutex_);
        t = std::move(tree_);
        tree_.reset();
    }
    if (!t) {
        log::logf(LogLevel::Warn, "Tried to stop the renderer when it was not started.");
        return;
    }
    // G2: the retrieval queue / request module must already be stopped (the caller's duty), then the tree closes.
    t->close();
    std::lock_guard lock(mutex_);
    retired_.push_back(std::move(t)); // keep alive: sections hold a reference to their tree (see header)
}

bool ClientLevelModule::isRendering() const {
    std::lock_guard lock(mutex_);
    return tree_ != nullptr;
}

std::shared_ptr<tree::LodQuadTree> ClientLevelModule::tree() const {
    std::lock_guard lock(mutex_);
    return tree_;
}

void ClientLevelModule::clientTick(BlockPos2D playerPos) {
    std::shared_ptr<tree::LodQuadTree> t;
    {
        std::lock_guard lock(mutex_);
        lastPlayerPos_ = playerPos;
        t              = tree_;
    }
    if (!t) return; // not rendering (Java: ClientRenderStateRef == null)

    // recreate the tree if the render distance changed (ClientLevelModule.java:75-87)
    if (t->blockRenderDistanceDiameter != tree::LodQuadTree::viewDiameterInBlocks(*deps_.options)) {
        t->close();
        std::lock_guard lock(mutex_);
        retired_.push_back(t);
        createTree(playerPos);
        t = tree_;
    }
    t->tryTick(playerPos);
    provider_.setCanRegenerate(t->canRegenerate()); // Java LodQuadTree: fullDataSourceProvider.setCanRegenerate(...)
}

void ClientLevelModule::reloadPos(sec::Pos pos) {
    if (auto t = tree()) t->queueReload(pos);
}

void ClientLevelModule::clearRenderCache(bool clearExistingData) {
    if (ctx_.level != nullptr) ctx_.level->clearBlockColorCache();
    if (auto t = tree()) t->clearRenderDataCache(clearExistingData);
}

void ClientLevelModule::onGeneratorPlanChanged(bool generationEnabled) {
    if (auto t = tree()) t->onGeneratorPlanChanged(generationEnabled);
}

const std::vector<RenderListEntry>& ClientLevelModule::collectRenderList(const RenderListParams& paramsIn) {
    std::lock_guard                           lock(renderMutex_);
    static const std::vector<RenderListEntry> kEmpty;
    auto                                      t = tree();
    if (!t) return kEmpty;
    RenderListParams params = paramsIn;
    if (params.options == nullptr) params.options = deps_.options;
    renderList_.build(*t, params);
    return renderList_.entries();
}

void ClientLevelModule::close() {
    if (isRendering()) stopRenderer();
}

//==================//
// ServerLevelModule //
//==================//

ServerLevelModule::ServerLevelModule(
    const WorldDeps&           deps,
    ILevelHandle&              serverHandle,
    GeneratedProvider&         provider,
    IWorldGenCompleteListener& listener
)
: deps_(deps),
  provider_(provider) {
    const Options& o = *deps.options;

    if (deps.chunkGeneratorFactory) {
        chunkGen_ = deps.chunkGeneratorFactory(serverHandle);
        if (!chunkGen_) {
            error_ = "the platform chunk generator could not be created";
            return;
        }
        DhWorldGeneratorOptions wgo;
        wgo.plan                    = o.common.worldGenerator.generatorPlan;
        wgo.build.worldCompression  = o.common.lodBuilding.worldCompression;
        wgo.update.worldCompression = o.common.lodBuilding.worldCompression;
        worldGen_                   = std::make_unique<WorldGenerator>(*chunkGen_, wgo);
    }

    // Java ServerLevelModule.LodRequestState.createRetrievalQueue -> new WorldGenerationQueue(worldGenerator, level)
    LodRequestModule::QueueFactory factory = [this]() -> std::unique_ptr<IRetrievalQueue> {
        if (!worldGen_ || !deps_.exec.worldGen || !deps_.exec.queueThread) return nullptr;
        const Options& opts = *deps_.options;
        auto           q    = std::make_unique<GenQueue>(
            *worldGen_,
            *deps_.exec.queueThread,
            *deps_.exec.worldGen,
            std::max(1, opts.common.multiThreading.numberOfThreads),
            *deps_.clock
        );
        q->setGenerationMode(toGenerationMode(opts.common.worldGenerator.chunkGeneratorMode));
        provider_.setQueueLowestDataDetail(q->lowestDataDetail());
        return q;
    };
    requestModule_ = std::make_unique<LodRequestModule>(provider, listener, std::move(factory));

    // request handling over the network (AbstractDhServerLevel.registerNetworkHandlers)
    if (deps.serverNetwork != nullptr && deps.serverPlayers != nullptr && deps.exec.networkWork != nullptr) {
        mp::LevelInfo li;
        // PORTING.md level naming rule (bedrock_common/LevelNaming.h): levelName = sendLevelKeys ? levelKey : dimension
        li.dimension               = std::string(serverHandle.info().dimensionName());
        li.levelKey                = serverHandle.levelKey();
        li.levelName               = deps.serverPlayers->settings.sendLevelKeys ? li.levelKey : li.dimension;
        li.coordinateScale         = serverHandle.coordinateScale();
        li.internalServerGenerator = o.common.worldGenerator.chunkGeneratorMode == DistantGeneratorMode::InternalServer;
        handler_                   = std::make_unique<mp::ServerRequestHandler>(
            *deps.serverPlayers,
            provider,
            *deps.exec.networkWork,
            std::move(li),
            mp::ServerRequestHandler::BeaconSupplier{},
            codecFrom(o)
        );
        deps.serverNetwork->addLevel(*handler_);
        handlerRegistered_ = true;
    }
}

ServerLevelModule::~ServerLevelModule() { close(); }

void ServerLevelModule::tick() {
    if (requestModule_) requestModule_->tick();
}

GenQueue* ServerLevelModule::genQueue() {
    if (!requestModule_) return nullptr;
    return dynamic_cast<GenQueue*>(requestModule_->queue());
}

void ServerLevelModule::onOptionsChanged() {
    const Options& o = *deps_.options;
    if (worldGen_) worldGen_->setGeneratorPlan(o.common.worldGenerator.generatorPlan);
    if (GenQueue* q = genQueue()) {
        q->setNumThreads(std::max(1, o.common.multiThreading.numberOfThreads));
        q->setGenerationMode(toGenerationMode(o.common.worldGenerator.chunkGeneratorMode));
    }
}

void ServerLevelModule::close() {
    if (handlerRegistered_) {
        deps_.serverNetwork->removeLevel(*handler_);
        handlerRegistered_ = false;
    }
    // Java ServerLevelModule.close(): lodRequestModule.close() (queue + generator shutdown) first
    if (requestModule_) requestModule_->close();
    if (handler_) handler_->close();
}

BlockPos2D nextPlayerTarget(mp::ServerPlayers* players, const std::string& levelName, std::size_t& cursor) {
    if (players == nullptr) return BlockPos2D{0, 0};
    std::vector<std::shared_ptr<mp::ServerPlayerState>> inLevel;
    for (auto& p : players->all()) {
        if (p->levelName() == levelName) inLevel.push_back(p);
    }
    if (inLevel.empty()) return BlockPos2D{0, 0}; // Java: DhBlockPos2D.ZERO
    const auto& chosen = inLevel[cursor % inLevel.size()];
    ++cursor; // "put first player in back": the next call serves the next player
    return chosen->position();
}

//===============//
// DhClientLevel //
//===============//

/// Java RemoteFullDataSourceProvider.get(): the first access of a position per 20 minutes also asks the server
/// whether it has a newer copy (SyncOnLoadRequestQueue) and applies the answer through the updater.
class DhClientLevel::RemoteSource final : public tree::ISectionDataSource {
public:
    RemoteSource(GeneratedProvider& provider, mp::SyncOnLoadRequestQueue* sync, const IClock& clock)
    : provider_(provider),
      sync_(sync),
      clock_(clock) {}

    std::shared_ptr<const FullDataSource> get(sec::Pos pos) override {
        if (sync_ != nullptr && firstVisit(pos)) {
            if (std::optional<std::int64_t> ts = provider_.timestamp(pos)) {
                GeneratedProvider& provider = provider_;
                sync_->submitRequest(pos, *ts)->onComplete([&provider](const RetrievalResult& result) {
                    if (result.kind == RetrievalResult::Kind::Success && result.source) {
                        (void)provider.updateAsync(result.source);
                    }
                });
            }
        }
        return provider_.get(pos);
    }
    std::shared_ptr<const FullDataSource> getAdj(sec::Pos pos, Direction dir) override {
        Result<std::shared_ptr<FullDataSource>> r = provider_.getAdj(pos, dir);
        if (!r) return nullptr;
        return std::shared_ptr<const FullDataSource>(std::move(*r));
    }

private:
    bool firstVisit(sec::Pos pos) {
        const std::int64_t now = clock_.nowMillis();
        std::lock_guard    lock(mutex_);
        auto               it = visited_.find(pos);
        if (it != visited_.end() && it->second > now) return false;
        if (visited_.size() > 4096) { // Guava expireAfterWrite(20 min) stand-in: sweep when it grows
            for (auto i = visited_.begin(); i != visited_.end();)
                i = i->second <= now ? visited_.erase(i) : std::next(i);
        }
        visited_[pos] = now + kSyncVisitedExpiryMs;
        return true;
    }

    GeneratedProvider&               provider_;
    mp::SyncOnLoadRequestQueue*      sync_;
    const IClock&                    clock_;
    std::mutex                       mutex_;
    std::map<sec::Pos, std::int64_t> visited_;
};

DhClientLevel::DhClientLevel(ILevelHandle& clientHandle, const WorldDeps& deps, mp::ClientNetworkState* net)
: AbstractLevel(clientHandle, deps),
  net_(net) {
    if (!ok()) return;
    if (!clientHandle.isClientSide() || clientHandle.clientLevel() == nullptr) {
        fail("a client level needs a client level handle");
        return;
    }
    if (net_ != nullptr && deps.exec.networkWork == nullptr) {
        fail("networking needs the network-work executor");
        return;
    }
    const std::string id = identifier();

    if (net_ != nullptr) {
        syncQueue_ = std::make_unique<mp::SyncOnLoadRequestQueue>(*net_, id, *deps.exec.networkWork);
        source_    = std::make_unique<RemoteSource>(provider(), syncQueue_.get(), *deps.clock);
    } else {
        source_ = std::make_unique<tree::ProviderSectionSource>(provider());
    }
    // Java RemoteFullDataSourceProvider.canQueueRetrievalNow() == canQueueRetrievalNow(true)
    client_ = std::make_unique<ClientLevelModule>(deps, provider(), *source_, true);

    // RemoteWorldRetrievalQueue as the provider's retrieval queue while world gen (downloads) is allowed
    requestModule_ =
        std::make_unique<LodRequestModule>(provider(), *this, [this]() -> std::unique_ptr<IRetrievalQueue> {
            if (net_ == nullptr) return nullptr;
            return std::make_unique<mp::ClientRequestQueue>(*net_, identifier(), *deps_.exec.networkWork);
        });

    if (net_ != nullptr) {
        applyPlan();
        std::weak_ptr<bool> weak = alive_;
        configListener_          = net_->addSessionConfigListener([this, weak] {
            if (auto a = weak.lock()) {
                if (!isClosed()) {
                    applyPlan();
                    client_->onGeneratorPlanChanged(generationEnabled(provider().generatorPlan()));
                }
            }
        });
        // real-time updates (DhClientLevel.registerNetworkHandlers: FullDataPartialUpdateMessage)
        net_->setPartialUpdateHandler(id, [this, weak](Result<FullDataSource> r, const std::vector<net::BeaconBeam>&) {
            auto a = weak.lock();
            if (!a || isClosed()) return;
            if (!r) {
                log::logf(LogLevel::Error, "Error while updating full data source: %s", r.error().message.c_str());
                return;
            }
            (void)provider().updateAsync(std::make_shared<const FullDataSource>(std::move(*r)));
        });
    }

    startRenderer(); // Java: clientside.startRenderer() at the end of the constructor
}

DhClientLevel::~DhClientLevel() { close(); }

void DhClientLevel::applyPlan() {
    // Java RemoteFullDataSourceProvider.getGeneratorPlan(): the server's constrained plan when connected
    provider().setGeneratorPlan(
        net_ != nullptr ? toConfigPlan(net_->sessionConfig().generatorPlan)
                        : deps_.options->common.worldGenerator.generatorPlan
    );
}

bool DhClientLevel::startRenderer() {
    IClientLevel* info = handle_.clientLevel();
    if (info == nullptr || !deps_.sinkFactory) {
        log::logf(LogLevel::Warn, "Unable to start the renderer for [%s]: no vertex sink.", identifier().c_str());
        return false;
    }
    IVertexSink* sink = deps_.sinkFactory(handle_);
    if (sink == nullptr) return false;
    return client_->startRenderer(*sink, *info);
}

void DhClientLevel::clientTick() {
    if (isClosed()) return;
    // can be false while the level is unloading / the player is not in the world (Java playerExists())
    std::optional<BlockPos2D> pos = deps_.playerBlockPos ? deps_.playerBlockPos() : std::nullopt;
    if (!pos) return;
    // only tick the level the player is currently in (DhClientLevel.java clientTick)
    if (deps_.activeClientLevel && deps_.activeClientLevel() != &handle_) return;

    lastPlayerX_.store(pos->x);
    lastPlayerZ_.store(pos->z);
    client_->clientTick(*pos);
    if (syncQueue_) syncQueue_->tick(*pos);
}

void DhClientLevel::tickImpl(std::int64_t nowMs) {
    if (nowMs - lastClientTickMs_ >= kClientTickMs) {
        lastClientTickMs_ = nowMs;
        clientTick();
    }
    requestModule_->tick();
    if (nowMs - lastExpiryMs_ >= kSaveCacheCleanupMs) {
        lastExpiryMs_           = nowMs;
        const std::int64_t wall = deps_.clock->nowMillis();
        std::lock_guard    lock(loadedOnceMutex_);
        for (auto it = loadedOnce_.begin(); it != loadedOnce_.end();)
            it = it->second <= wall ? loadedOnce_.erase(it) : std::next(it);
    }
}

bool DhClientLevel::shouldDoWorldGen() {
    if (net_ == nullptr || !net_->isReady()) return false;
    if (!generationEnabled(toConfigPlan(net_->sessionConfig().generatorPlan))) return false;
    const bool allowedDimension = !deps_.activeClientLevel || deps_.activeClientLevel() == &handle_;
    return allowedDimension && client_->isRendering();
}

std::optional<BlockPos2D> DhClientLevel::targetPosForGeneration() {
    if (deps_.playerBlockPos) {
        if (auto p = deps_.playerBlockPos()) return p;
    }
    return BlockPos2D{lastPlayerX_.load(), lastPlayerZ_.load()};
}

void DhClientLevel::onWorldGenTaskComplete(std::uint64_t pos) { client_->reloadPos(pos); }

BlockPos2D DhClientLevel::focusPos() const { return BlockPos2D{lastPlayerX_.load(), lastPlayerZ_.load()}; }

bool DhClientLevel::shouldProcessChunkUpdate(ChunkPos pos) {
    if (net_ == nullptr || !net_->isReady()) return true;
    if (!net_->sessionConfig().enableRealTimeUpdates) return true;
    const std::int64_t now = deps_.clock->nowMillis();
    std::lock_guard    lock(loadedOnceMutex_);
    auto               it = loadedOnce_.find({pos.x, pos.z});
    if (it != loadedOnce_.end() && it->second > now) return false; // the server keeps this chunk up to date
    loadedOnce_[{pos.x, pos.z}] = now + kLoadedOnceExpiryMs;
    return true;
}

void DhClientLevel::optionsChangedImpl() {
    if (net_ != nullptr) applyPlan();
}

void DhClientLevel::closeImpl() {
    // Java DhClientLevel.close(): lodRequestModule, network source, session config listener, clientside, then super
    if (requestModule_) requestModule_->close();
    alive_.reset();
    if (net_ != nullptr) {
        net_->setPartialUpdateHandler(identifier(), {});
        if (configListener_ != 0) net_->removeSessionConfigListener(configListener_);
        configListener_ = 0;
    }
    if (syncQueue_) syncQueue_->closeQueue();
    if (client_) client_->close();
}

//=====================//
// DhClientServerLevel //
//=====================//

DhClientServerLevel::DhClientServerLevel(ILevelHandle& serverHandle, const WorldDeps& deps)
: AbstractLevel(serverHandle, deps) {
    if (!ok()) return;
    source_ = std::make_unique<tree::ProviderSectionSource>(provider());
    client_ = std::make_unique<ClientLevelModule>(deps, provider(), *source_, false);
    server_ = std::make_unique<ServerLevelModule>(deps, serverHandle, provider(), *this);
    if (!server_->ok()) fail(server_->error());
}

DhClientServerLevel::~DhClientServerLevel() { close(); }

bool DhClientServerLevel::startRenderer(ILevelHandle& clientHandle) {
    {
        std::lock_guard lock(handlesMutex_);
        if (std::find(clientHandles_.begin(), clientHandles_.end(), &clientHandle) == clientHandles_.end()) {
            clientHandles_.push_back(&clientHandle);
        }
    }
    if (client_->isRendering()) return true;
    IClientLevel* info = clientHandle.clientLevel();
    if (info == nullptr || !deps_.sinkFactory) {
        log::logf(LogLevel::Warn, "Unable to start the renderer for [%s]: no vertex sink.", identifier().c_str());
        return false;
    }
    IVertexSink* sink = deps_.sinkFactory(clientHandle);
    if (sink == nullptr) return false;
    return client_->startRenderer(*sink, *info);
}

void DhClientServerLevel::forgetClientHandle(ILevelHandle& clientHandle) {
    std::lock_guard lock(handlesMutex_);
    clientHandles_.erase(
        std::remove(clientHandles_.begin(), clientHandles_.end(), &clientHandle),
        clientHandles_.end()
    );
}

std::size_t DhClientServerLevel::clientHandleCount() const {
    std::lock_guard lock(handlesMutex_);
    return clientHandles_.size();
}

void DhClientServerLevel::stopRenderer() {
    if (client_->isRendering()) client_->stopRenderer();
}

void DhClientServerLevel::clientTick() {
    if (isClosed()) return;
    std::optional<BlockPos2D> pos = deps_.playerBlockPos ? deps_.playerBlockPos() : std::nullopt;
    if (!pos) return;
    if (deps_.activeClientLevel) {
        // DhClientServerLevel.java clientTick: only the level whose client wrapper the player is in
        ILevelHandle*   active = deps_.activeClientLevel();
        std::lock_guard lock(handlesMutex_);
        if (std::find(clientHandles_.begin(), clientHandles_.end(), active) == clientHandles_.end()) return;
    }
    lastPlayerX_.store(pos->x);
    lastPlayerZ_.store(pos->z);
    client_->clientTick(*pos);
}

void DhClientServerLevel::tickImpl(std::int64_t nowMs) {
    if (nowMs - lastClientTickMs_ >= kClientTickMs) {
        lastClientTickMs_ = nowMs;
        clientTick();
    }
    server_->tick();
}

bool DhClientServerLevel::shouldDoWorldGen() {
    return generationEnabled(deps_.options->common.worldGenerator.generatorPlan) && server_ && server_->canGenerate();
}

std::optional<BlockPos2D> DhClientServerLevel::targetPosForGeneration() {
    if (deps_.serverPlayers != nullptr) {
        std::vector<std::shared_ptr<mp::ServerPlayerState>> any = deps_.serverPlayers->all();
        if (!any.empty()) return nextPlayerTarget(deps_.serverPlayers, identifier(), playerCursor_);
    }
    if (deps_.playerBlockPos) {
        if (auto p = deps_.playerBlockPos()) return p;
    }
    return BlockPos2D{lastPlayerX_.load(), lastPlayerZ_.load()};
}

void DhClientServerLevel::onWorldGenTaskComplete(std::uint64_t pos) {
    if (server_ && server_->requestHandler() != nullptr) server_->requestHandler()->onWorldGenTaskComplete(pos);
    client_->reloadPos(pos); // DhClientServerLevel.java onWorldGenTaskComplete
}

BlockPos2D DhClientServerLevel::focusPos() const { return BlockPos2D{lastPlayerX_.load(), lastPlayerZ_.load()}; }

void DhClientServerLevel::optionsChangedImpl() {
    if (server_) server_->onOptionsChanged();
    client_->onGeneratorPlanChanged(generationEnabled(deps_.options->common.worldGenerator.generatorPlan));
}

void DhClientServerLevel::closeImpl() {
    // G2 rule: stop everything that calls back into the tree (world gen, request handler) BEFORE closing the tree
    if (server_) server_->close();
    if (client_) client_->close();
}

//===============//
// DhServerLevel //
//===============//

DhServerLevel::DhServerLevel(ILevelHandle& serverHandle, const WorldDeps& deps) : AbstractLevel(serverHandle, deps) {
    if (!ok()) return;
    server_ = std::make_unique<ServerLevelModule>(deps, serverHandle, provider(), *this);
    if (!server_->ok()) fail(server_->error());
}

DhServerLevel::~DhServerLevel() { close(); }

bool DhServerLevel::shouldDoWorldGen() {
    return generationEnabled(deps_.options->common.worldGenerator.generatorPlan) && server_ && server_->canGenerate();
}

std::optional<BlockPos2D> DhServerLevel::targetPosForGeneration() {
    return nextPlayerTarget(deps_.serverPlayers, identifier(), playerCursor_);
}

void DhServerLevel::onWorldGenTaskComplete(std::uint64_t pos) {
    if (server_ && server_->requestHandler() != nullptr) server_->requestHandler()->onWorldGenTaskComplete(pos);
}

BlockPos2D DhServerLevel::focusPos() const {
    std::size_t cursor = 0;
    return nextPlayerTarget(deps_.serverPlayers, identifier(), cursor);
}

void DhServerLevel::tickImpl(std::int64_t) { server_->tick(); }

void DhServerLevel::optionsChangedImpl() {
    if (server_) server_->onOptionsChanged();
}

void DhServerLevel::closeImpl() {
    if (server_) server_->close();
}

} // namespace dh
