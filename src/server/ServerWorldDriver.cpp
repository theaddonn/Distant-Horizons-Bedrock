#include "server/ServerWorldDriver.h"

#include "bedrock_common/BdsWorldGenerator.h"
#include "bedrock_common/BedrockChunk.h"
#include "bedrock_common/BedrockLevelInfo.h"
#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "bedrock_common/KvChunkHashStore.h"
#include "bedrock_common/LevelDbLodStore.h"
#include "server/NetServer.h"

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/fulldata/Merge.h"
#include "dh/gen/GenQueue.h"
#include "dh/gen/GeneratedProvider.h"
#include "dh/gen/LodRequestModule.h"
#include "dh/gen/WorldGenerator.h"
#include "dh/storage/ChunkHashStore.h"
#include "dh/storage/UpdatePropagator.h"

#include "ll/api/memory/Hook.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/DBChunkStorage.h"
#include "mc/world/level/storage/LevelData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>

namespace dh::bedrock {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSlowTick          = std::chrono::milliseconds(100);
constexpr auto kIngestBudget      = std::chrono::milliseconds(10);
constexpr int  kIngestBatchChunks = 64;
constexpr int  kMaxNotLoadedTries = 20;

std::atomic<ServerWorldDriver*> gDriver{nullptr};
std::atomic<int>                gInHook{0};

struct HookGuard {
    HookGuard() { ++gInHook; }
    ~HookGuard() { --gInHook; }
};

LL_TYPE_INSTANCE_HOOK(
    ServerOnChunkLoadedHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$onChunkLoaded,
    void,
    ::ChunkSource& source,
    ::LevelChunk&  lc
) {
    origin(source, lc);
    try {
        HookGuard g;
        if (auto* d = gDriver.load()) d->noteChunk(lc);
    } catch (...) {
        DH_LOG_E("server driver: exception swallowed in onChunkLoaded hook");
    }
}

// DBChunkStorage is the server's persistent chunk source: a dirty live chunk is written here, which is our
// "block changed" signal (the chunk hash makes unchanged saves free). TODO(verify in game): call frequency.
LL_TYPE_INSTANCE_HOOK(
    ServerOnSaveLiveChunkHook,
    ll::memory::HookPriority::Normal,
    DBChunkStorage,
    &DBChunkStorage::$saveLiveChunk,
    bool,
    ::LevelChunk& lc
) {
    const bool r = origin(lc);
    try {
        HookGuard g;
        if (auto* d = gDriver.load()) d->noteChunk(lc);
    } catch (...) {
        DH_LOG_E("server driver: exception swallowed in saveLiveChunk hook");
    }
    return r;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream     in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct PendingKey {
    int         dim = 0, x = 0, z = 0;
    friend bool operator<(const PendingKey& a, const PendingKey& b) {
        return std::tie(a.dim, a.x, a.z) < std::tie(b.dim, b.x, b.z);
    }
};

dh::GenerationMode toGenerationMode(dh::DistantGeneratorMode m) {
    return static_cast<dh::GenerationMode>(dh::complexity(m));
}

} // namespace

namespace {

/// IWorldGenCompleteListener for one dimension: world gen runs while a ready player is in it.
class DimListener final : public dh::IWorldGenCompleteListener {
public:
    DimListener(dh::mp::ServerPlayers& players, std::string levelName, const std::atomic<bool>& generationAllowed)
    : players_(players),
      levelName_(std::move(levelName)),
      generationAllowed_(generationAllowed) {}

    /// Set once the request handler exists (before any world gen can complete).
    std::atomic<dh::mp::ServerRequestHandler*> handler{nullptr};

    bool shouldDoWorldGen() override {
        if (!generationAllowed_.load()) return false; // not players_.settings: that is written on another thread
        for (const auto& p : players_.readyPlayers())
            if (p->levelName() == levelName_) return true;
        return false;
    }
    std::optional<dh::BlockPos2D> targetPosForGeneration() override {
        for (const auto& p : players_.readyPlayers())
            if (p->levelName() == levelName_) return p->position();
        return std::nullopt;
    }
    void onWorldGenTaskComplete(std::uint64_t pos) override {
        if (auto* h = handler.load()) h->onWorldGenTaskComplete(pos);
    }

private:
    dh::mp::ServerPlayers&   players_;
    std::string              levelName_;
    const std::atomic<bool>& generationAllowed_;
};

} // namespace

//=====//
// Dim //
//=====//

struct ServerWorldDriver::Dim {
    int               id        = 0;
    Dimension*        dimension = nullptr;
    std::string       resource; ///< "minecraft:overworld"
    bool              hasSkyLight = true;
    dh::mp::LevelInfo info;

    // construction order == reverse destruction order (see "Shutdown order" in the header)
    std::unique_ptr<LevelDbLodStore>              store;
    std::unique_ptr<KvChunkHashStore>             hashStore;
    std::unique_ptr<dh::ChunkHashTracker>         tracker;
    std::unique_ptr<dh::GeneratedProvider>        provider;
    std::unique_ptr<dh::UpdatePropagator>         propagator;
    std::unique_ptr<BdsWorldGenerator>            chunkGen;
    std::unique_ptr<dh::WorldGenerator>           worldGen;
    std::unique_ptr<DimListener>                  listener;
    std::unique_ptr<dh::LodRequestModule>         module;
    std::unique_ptr<dh::mp::ServerRequestHandler> handler;

    std::mutex              inflightMutex;
    std::set<std::uint64_t> inflight; ///< sections with an update in flight (hash commit is per section)
    dh::BlockPos2D          lastTarget;
};

//======//
// Impl //
//======//

struct ServerWorldDriver::Impl {
    explicit Impl(ServerWorldDriver& s) : self(s) {}

    ServerWorldDriver& self;
    dh::SystemClock    clock;
    dh::ConfigStore*   config = nullptr;
    std::string        worldName;

    std::optional<dh::BlockTable>    table;
    std::unique_ptr<BlockSerializer> serializer;
    std::unique_ptr<dh::ThreadPools> pools;
    dh::SingleThreadExecutor         queueExec{"DH WorldGen Queue"};
    PacketTransport                  transport;
    std::unique_ptr<NetServer>       net;

    std::vector<std::unique_ptr<Dim>> dims;       // server thread only
    std::set<int>                     failedDims; // dimensions we could not set up (logged once)

    mutable std::mutex        pendingMutex;
    std::map<PendingKey, int> pending; ///< chunk -> failed "not loaded yet" attempts

    std::atomic<std::uint64_t> noted{0}, ingested{0}, unchanged{0}, skipped{0}, sectionsOk{0}, sectionsFailed{0};

    bool                      hooksInstalled = false, saveHookInstalled = false;
    Clock::time_point         lastTickTime = Clock::now();
    std::chrono::milliseconds accum{0};
    Clock::time_point         lastSlow           = Clock::now();
    Clock::time_point         lastLog            = Clock::now();
    std::uint64_t             lastLoggedNoted    = 0;
    std::uint64_t             lastLoggedIngested = 0;

    // --- dimension setup -------------------------------------------------------------------------

    Dim* findDim(int id) {
        for (auto& d : dims)
            if (d->id == id) return d.get();
        return nullptr;
    }

    Dim* ensureDim(int id) {
        if (Dim* d = findDim(id)) return d;
        if (failedDims.contains(id)) return nullptr;
        try {
            Dim* d = createDim(id);
            if (!d) failedDims.insert(id);
            return d;
        } catch (...) {
            failedDims.insert(id);
            DH_LOG_E("server driver: exception while creating dimension %d", id);
            return nullptr;
        }
    }

    Dim* createDim(int id);
    void destroyDim(Dim& d);
    void ensureAllDims();
    void ingestPending();
    void updatePlayers();
    void tickDims();
    void logPeriodic();
};

ServerWorldDriver::Dim* ServerWorldDriver::Impl::createDim(int id) {
    Dimension* found = nullptr;
    self.level_.forEachDimension([&](Dimension& d) -> bool {
        if (d.mId->mValue == id) {
            found = &d;
            return false;
        }
        return true;
    });
    if (!found) {
        DH_LOG_W("server driver: dimension %d not found", id);
        return nullptr;
    }
    const dh::LevelInfo li = levelInfoOf(*found);
    if (li.maxHeight <= li.minHeight || (li.maxHeight - li.minHeight) % 16 != 0) {
        DH_LOG_W("server driver: dimension %s has an unsupported height range, skipped", li.dimensionName.c_str());
        return nullptr;
    }

    const dh::Options& opts = config->options();
    auto               dim  = std::make_unique<Dim>();
    dim->id                 = id;
    dim->dimension          = found;
    dim->resource           = li.dimensionName;
    dim->hasSkyLight        = li.hasSkyLight;
    dim->info               = NetServer::levelInfoFor(worldName, li.dimensionName, id == 1 ? 8.0 : 1.0, opts);

    const auto      dir = self.paths_.worldDataDir(worldName) / sanitizePathComponent(li.dimensionName);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    auto store = LevelDbLodStore::open(dir / "lods");
    if (!store) {
        DH_LOG_E(
            "server driver: cannot open LOD store for %s: %s",
            li.dimensionName.c_str(),
            store.error().message.c_str()
        );
        return nullptr;
    }
    dim->store = std::move(*store);
    auto kv    = LeviKeyValue::open(dir / "chunkhash");
    if (!kv) {
        DH_LOG_E(
            "server driver: cannot open chunk hash DB for %s: %s",
            li.dimensionName.c_str(),
            kv.error().message.c_str()
        );
        return nullptr;
    }
    dim->hashStore = std::make_unique<KvChunkHashStore>(std::move(*kv));
    dim->tracker   = std::make_unique<dh::ChunkHashTracker>(*dim->hashStore);

    const int threads = opts.common.multiThreading.numberOfThreads;

    // Codec compression stays None until zstd is vendored (Codec.cpp returns Unsupported for Zstd).
    dh::CodecOptions codec;
    codec.compression = dh::Compression::None;

    dh::ProviderOptions po;
    po.update.worldCompression = opts.common.lodBuilding.worldCompression;
    po.maxSkyLight             = li.hasSkyLight ? 15 : 0;

    dh::GeneratedProviderOptions go;
    go.plan            = opts.common.worldGenerator.generatorPlan;
    go.numberOfThreads = threads;

    dim->provider =
        std::make_unique<dh::GeneratedProvider>(*dim->store, *table, clock, pools->fileHandler(), codec, po, go);
    Dim* raw = dim.get();
    dim->provider->setQueuedChunkUpdateCounter([this] {
        std::lock_guard lock(pendingMutex);
        return static_cast<int>(pending.size());
    });
    dim->propagator = std::make_unique<dh::UpdatePropagator>(*dim->provider, pools->updatePropagator(), threads);

    dim->chunkGen = std::make_unique<BdsWorldGenerator>(
        *found,
        *serializer,
        pools->worldGen(),
        opts.common.worldGenerator.scratchThreads
    );
    dh::DhWorldGeneratorOptions wo;
    wo.plan                    = opts.common.worldGenerator.generatorPlan;
    wo.build.worldCompression  = opts.common.lodBuilding.worldCompression;
    wo.update.worldCompression = opts.common.lodBuilding.worldCompression;
    dim->worldGen              = std::make_unique<dh::WorldGenerator>(*dim->chunkGen, wo);

    dim->listener = std::make_unique<DimListener>(net->players(), dim->info.levelName, net->generationAllowed());
    dim->module   = std::make_unique<dh::LodRequestModule>(
        *dim->provider,
        *dim->listener,
        [this, raw, threads, mode = toGenerationMode(opts.common.worldGenerator.chunkGeneratorMode)] {
            auto q = std::make_unique<dh::GenQueue>(*raw->worldGen, queueExec, pools->worldGen(), threads, clock);
            q->setGenerationMode(mode);
            raw->provider->setQueueLowestDataDetail(q->lowestDataDetail());
            return q;
        }
    );

    dim->handler = std::make_unique<dh::mp::ServerRequestHandler>(
        net->players(),
        *dim->provider,
        pools->networkCompression(),
        dim->info,
        dh::mp::ServerRequestHandler::BeaconSupplier{},
        codec
    );
    dim->listener->handler.store(dim->handler.get());
    net->network().addLevel(*dim->handler);

    DH_LOG_I(
        "server driver: dimension %s ready (level '%s', %d..%d, sky=%d, store %s)",
        li.dimensionName.c_str(),
        dim->info.levelName.c_str(),
        li.minHeight,
        li.maxHeight,
        li.hasSkyLight ? 1 : 0,
        dir.string().c_str()
    );
    dims.push_back(std::move(dim));
    return raw;
}

void ServerWorldDriver::Impl::ensureAllDims() {
    std::vector<int> ids;
    self.level_.forEachDimension([&](Dimension& d) -> bool {
        ids.push_back(d.mId->mValue);
        return true;
    });
    for (int id : ids) (void)ensureDim(id); // handlers must exist before the first RequestLevelInit
}

void ServerWorldDriver::Impl::destroyDim(Dim& d) {
    if (d.handler) {
        net->network().removeLevel(*d.handler);
        d.handler->close();
    }
    if (d.module) d.module->close();
    if (d.worldGen) d.worldGen->close();
    // Callbacks of in-flight chunks capture dh::WorldGenerator (destroyed before chunkGen): drain the workers first.
    if (d.chunkGen) d.chunkGen->stopWorkersAndWait();
}

//=========//
// ingest  //
//=========//

void ServerWorldDriver::Impl::ingestPending() {
    std::vector<std::pair<PendingKey, int>> batch;
    {
        std::lock_guard lock(pendingMutex);
        auto            it = pending.begin();
        while (it != pending.end() && static_cast<int>(batch.size()) < kIngestBatchChunks) {
            batch.emplace_back(it->first, it->second);
            it = pending.erase(it);
        }
    }
    if (batch.empty()) return;

    const dh::Options& opts       = config->options();
    const bool         skipHashes = opts.common.lodBuilding.disableUnchangedChunkCheck;
    const auto         deadline   = Clock::now() + kIngestBudget;

    struct Cached {
        std::shared_ptr<LevelChunk>   lc;
        std::unique_ptr<BedrockChunk> wrapper;
    };
    std::map<std::tuple<int, int, int>, Cached> cache; // lives for this tick only (chunks may change afterwards)

    enum class Fetch { Ok, NotLoaded, Unsupported, Missing };
    auto fetch = [&](Dim& d, int x, int z, BedrockChunk*& out) -> Fetch {
        out      = nullptr;
        auto key = std::make_tuple(d.id, x, z);
        if (auto it = cache.find(key); it != cache.end()) {
            out = it->second.wrapper.get();
            return out ? Fetch::Ok : Fetch::Unsupported;
        }
        std::shared_ptr<LevelChunk> lc = d.dimension->getChunkSource().getAvailableChunk(::ChunkPos(x, z));
        if (!lc) return Fetch::Missing;
        if (lc->mLoadState->load() < ChunkState::Loaded) return Fetch::NotLoaded;
        Cached c;
        c.lc = std::move(lc);
        if (BedrockChunk::heightSupported(*c.lc)) c.wrapper = std::make_unique<BedrockChunk>(*c.lc, *serializer);
        out = c.wrapper.get();
        cache.emplace(key, std::move(c));
        return out ? Fetch::Ok : Fetch::Unsupported;
    };

    // group by (dimension, section) so every section is updated once (ChunkHashStore.h step 3)
    std::map<std::pair<int, std::uint64_t>, std::vector<std::pair<PendingKey, int>>> groups;
    for (auto& [k, tries] : batch) groups[{k.dim, dh::sectionPosForChunk(k.x, k.z)}].emplace_back(k, tries);

    dh::LightingOptions lo;
    lo.maxSkyLight        = 15;
    lo.generateBlockLight = true;
    lo.generateSkyLight   = false; // baked per section by the updater at detail 6
    dh::BuildOptions bo;
    bo.worldCompression = opts.common.lodBuilding.worldCompression;
    dh::UpdateOptions uo;
    uo.worldCompression = opts.common.lodBuilding.worldCompression;

    auto requeue = [&](const PendingKey& k, int tries) {
        std::lock_guard lock(pendingMutex);
        pending.emplace(k, tries); // keeps an already re-noted entry's count
    };

    for (auto& [gkey, members] : groups) {
        if (Clock::now() > deadline) {
            for (auto& [k, tries] : members) requeue(k, tries);
            continue;
        }
        Dim* d = ensureDim(gkey.first);
        if (!d) {
            skipped += members.size();
            continue;
        }
        const std::uint64_t secPos = gkey.second;
        {
            std::lock_guard lock(d->inflightMutex);
            if (d->inflight.contains(secPos)) { // an earlier update of this section is still being saved
                for (auto& [k, tries] : members) requeue(k, tries);
                continue;
            }
        }

        std::optional<dh::FullDataSource> acc;
        std::vector<dh::ChunkHashEntry>   entries;
        for (auto& [k, tries] : members) {
            BedrockChunk* bc = nullptr;
            switch (fetch(*d, k.x, k.z, bc)) {
            case Fetch::NotLoaded:
                if (tries + 1 < kMaxNotLoadedTries) requeue(k, tries + 1);
                else ++skipped;
                continue;
            case Fetch::Missing:
            case Fetch::Unsupported:
                ++skipped;
                continue;
            case Fetch::Ok:
                break;
            }
            std::int32_t hash = static_cast<std::int32_t>(bc->blockBiomeHashCode());
            if (hash == 0) hash = 1; // 0 means "unknown" in the hash store
            if (!skipHashes && d->hashStore->get({k.x, k.z}) == hash) {
                ++unchanged;
                continue;
            }

            // block light over the 3x3 neighbourhood (neighbours that are not loaded are simply absent)
            std::vector<dh::IChunk*> nearby;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    BedrockChunk* n = nullptr;
                    if (fetch(*d, k.x + dx, k.z + dz, n) == Fetch::Ok) nearby.push_back(n);
                }
            dh::lightChunk(*bc, nearby, lo);
            auto src = dh::createFromChunk(*bc, bo);
            if (!src) {
                ++skipped;
                continue;
            }
            if (!acc) acc = std::move(*src);
            else (void)dh::updateFromDataSource(*acc, *src, uo);
            entries.push_back({
                {k.x, k.z},
                hash
            });
        }
        if (!acc) continue;

        for (const auto& e : entries) d->tracker->notePending(secPos, e.pos, e.hash);
        {
            std::lock_guard lock(d->inflightMutex);
            d->inflight.insert(secPos);
        }
        ingested += entries.size();
        auto fut  = d->provider->updateAsync(std::make_shared<const dh::FullDataSource>(std::move(*acc)));
        fut->onComplete([this, d, secPos](const dh::Result<bool>& r) {
            // ChunkHashStore.h: commit only on success and only if the provider is still open.
            if (r.has_value() && !d->provider->isClosed()) {
                d->tracker->commit(secPos);
                ++sectionsOk;
            } else {
                d->tracker->discard(secPos);
                ++sectionsFailed;
            }
            std::lock_guard lock(d->inflightMutex);
            d->inflight.erase(secPos);
        });
    }
}

void ServerWorldDriver::Impl::updatePlayers() {
    dh::mp::ServerPlayers& players = net->players();
    self.level_.forEachPlayer([&](Player& p) -> bool {
        const dh::SessionId sid = transport.sessionOf(p.getNetworkIdentifier());
        if (sid == 0) return true;
        Dim* d = ensureDim(static_cast<int>(p.getDimensionId()));
        if (!d) return true;
        const auto& pos = p.getPosition();
        d->lastTarget =
            dh::BlockPos2D{static_cast<std::int32_t>(std::floor(pos.x)), static_cast<std::int32_t>(std::floor(pos.z))};
        players.setPosition(sid, d->info.levelName, d->lastTarget);
        return true;
    });
}

void ServerWorldDriver::Impl::tickDims() {
    for (auto& d : dims) {
        d->module->tick();
        d->propagator->tick(d->lastTarget);
    }
}

void ServerWorldDriver::Impl::logPeriodic() {
    const auto now = Clock::now();
    if (now - lastLog < std::chrono::seconds(10)) return;
    lastLog = now;
    if (ingested.load() == lastLoggedIngested && noted.load() == lastLoggedNoted) return; // nothing new
    lastLoggedIngested = ingested.load();
    lastLoggedNoted    = noted.load();
    DH_LOG_I("server driver: %s", self.statusText().c_str());
}

//=================//
// ServerWorldDriver
//=================//

ServerWorldDriver::ServerWorldDriver(Level& level, ModPaths paths, std::filesystem::path modDir)
: level_(level),
  paths_(std::move(paths)),
  modDir_(std::move(modDir)),
  impl_(std::make_unique<Impl>(*this)) {}

ServerWorldDriver::~ServerWorldDriver() { stop(); }

bool ServerWorldDriver::start() {
    if (running_.load()) return true;
    try {
        Impl& m  = *impl_;
        m.config = &ConfigBridge::instance().config();

        const auto dataDir = modDir_ / "data";
        auto table = dh::BlockTable::load(slurp(dataDir / "block_table.json"), slurp(dataDir / "biome_table.json"));
        if (!table) {
            DH_LOG_E(
                "server driver: cannot load block tables from %s: %s",
                dataDir.string().c_str(),
                table.error().message.c_str()
            );
            return false;
        }
        m.table.emplace(std::move(*table));
        m.serializer = std::make_unique<BlockSerializer>(*m.table);
        DH_LOG_I(
            "server driver: block table loaded (%zu blocks, %zu biomes)",
            m.table->blockCount(),
            m.table->biomeCount()
        );

        // Persist a random server id on first start (Java default is a random int; 0 = not yet chosen).
        if (m.config->options().server.serverId == 0) {
            std::random_device rd;
            std::int32_t       id = static_cast<std::int32_t>(rd());
            if (id == 0) id = 1;
            m.config->set([&](dh::Options& o) { o.server.serverId = id; });
        }

        const dh::Options& opts = m.config->options();
        dh::ThreadOptions  to;
        to.numberOfThreads = opts.common.multiThreading.numberOfThreads;
        to.runTimeRatio    = static_cast<float>(opts.common.multiThreading.threadRunTimeRatio);
        m.pools            = std::make_unique<dh::ThreadPools>(to);

        m.worldName = level_.getLevelData().mLevelName.get();
        if (m.worldName.empty()) m.worldName = "world";

        m.net = std::make_unique<NetServer>(m.transport, m.clock, *m.config);
        m.net->start();

        // LeviLamina convention: hook() returns 0 on success (TODO(verify in game)).
        if (const int r = ServerOnChunkLoadedHook::hook(); r != 0) {
            DH_LOG_E("server driver: hooking Level::onChunkLoaded failed (%d)", r);
            stop();
            return false;
        }
        m.hooksInstalled = true;
        if (const int r = ServerOnSaveLiveChunkHook::hook(); r != 0) {
            DH_LOG_E("server driver: hooking DBChunkStorage::saveLiveChunk failed (%d)", r);
            stop();
            return false;
        }
        m.saveHookInstalled = true;
        m.net->setJoinHook([&m] { m.ensureAllDims(); });
        m.ensureAllDims();
        gDriver.store(this);
        running_.store(true);
        DH_LOG_I(
            "server driver: started for world '%s' (%d worker threads, data %s)",
            m.worldName.c_str(),
            to.numberOfThreads,
            paths_.dataDir.string().c_str()
        );
        return true;
    } catch (...) {
        DH_LOG_E("server driver: exception during start");
        stop();
        return false;
    }
}

void ServerWorldDriver::stop() {
    Impl& m = *impl_;
    if (!running_.exchange(false) && !m.pools) return;
    try {
        // 1. hooks off; wait for threads already inside a hook
        gDriver.store(nullptr);
        if (m.hooksInstalled && !ServerOnChunkLoadedHook::unhook())
            DH_LOG_W("server driver: unhook onChunkLoaded failed");
        if (m.saveHookInstalled && !ServerOnSaveLiveChunkHook::unhook())
            DH_LOG_W("server driver: unhook saveLiveChunk failed");
        m.hooksInstalled = m.saveHookInstalled = false;
        while (gInHook.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard lock(m.pendingMutex);
            m.pending.clear();
        }
        // 2. no more network events; transport dispatch drained
        if (m.net) m.net->stop();
        m.transport.shutdown();
        // 3. request handlers, world-gen modules/queues/generator
        for (auto& d : m.dims) m.destroyDim(*d);
        // 4. executors: nothing may run after this point (providers/updaters capture `this`)
        if (m.pools) {
            m.pools->shutdown();
            m.pools->picker().joinAll();
        }
        m.queueExec.shutdownNow();
        m.queueExec.join();
        // 5. quiesce updaters, close providers
        for (auto& d : m.dims) {
            if (d->provider) {
                d->provider->updater().close();
                d->provider->updater().waitQuiescent();
                d->provider->flush();
                d->provider->close();
            }
        }
        // 6. destroy per-dimension state (reverse construction order), then network, tables, pools
        m.dims.clear();
        m.net.reset();
        m.serializer.reset();
        m.table.reset();
        m.pools.reset();
        DH_LOG_I("server driver: stopped");
    } catch (...) {
        DH_LOG_E("server driver: exception during stop");
    }
}

void ServerWorldDriver::onServerTick() {
    if (!running_.load()) return;
    Impl& m = *impl_;
    try {
        m.net->applyPendingConfig();
        m.net->network().tick(); // 20 Hz: request groups, payload senders (bandwidth maths assume 50 ms)
        m.transport.flushOutbound();

        // 100 ms cadence from an accumulator (no drift / jitter from tick granularity)
        const auto now  = Clock::now();
        m.accum        += std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m.lastTickTime),
            std::chrono::milliseconds(250)
        );
        m.lastTickTime = now;
        if (m.accum < kSlowTick) return;
        m.accum -= kSlowTick;
        if (m.accum >= kSlowTick) m.accum = std::chrono::milliseconds(0); // fell behind: do not burst

        m.updatePlayers();
        m.ingestPending();
        m.tickDims();
        m.logPeriodic();
    } catch (...) {
        DH_LOG_E("server driver: exception swallowed in server tick");
    }
}

void ServerWorldDriver::noteChunk(LevelChunk& chunk) {
    if (!running_.load()) return;
    PendingKey k;
    k.dim = chunk.mDimension.mId->mValue;
    k.x   = chunk.mPosition->x;
    k.z   = chunk.mPosition->z;
    ++impl_->noted;
    std::lock_guard lock(impl_->pendingMutex);
    impl_->pending.emplace(k, 0);
}

ServerDriverStats ServerWorldDriver::stats() const {
    ServerDriverStats s;
    const Impl&       m = *impl_;
    s.chunksNoted       = m.noted.load();
    s.chunksIngested    = m.ingested.load();
    s.chunksUnchanged   = m.unchanged.load();
    s.chunksSkipped     = m.skipped.load();
    s.sectionsUpdated   = m.sectionsOk.load();
    s.sectionsFailed    = m.sectionsFailed.load();
    {
        std::lock_guard lock(m.pendingMutex);
        s.pendingChunks = m.pending.size();
    }
    s.dimensions     = m.dims.size(); // approximate off the server thread (status only)
    s.players        = m.transport.sessionCount();
    s.framesSent     = m.transport.framesSent();
    s.framesReceived = m.transport.framesReceived();
    for (const auto& d : m.dims) {
        if (d->chunkGen) {
            s.generatedChunks       += d->chunkGen->generatedChunks();
            s.failedGeneratedChunks += d->chunkGen->failedChunks();
        }
    }
    return s;
}

std::string ServerWorldDriver::statusText() const {
    const ServerDriverStats s = stats();
    char                    buf[512];
    std::snprintf(
        buf,
        sizeof buf,
        "dims=%zu players=%zu chunks: noted=%llu ingested=%llu unchanged=%llu skipped=%llu pending=%zu | sections: "
        "ok=%llu failed=%llu | "
        "worldgen: chunks=%llu failed=%llu | net: tx=%llu rx=%llu frames",
        s.dimensions,
        s.players,
        (unsigned long long)s.chunksNoted,
        (unsigned long long)s.chunksIngested,
        (unsigned long long)s.chunksUnchanged,
        (unsigned long long)s.chunksSkipped,
        s.pendingChunks,
        (unsigned long long)s.sectionsUpdated,
        (unsigned long long)s.sectionsFailed,
        (unsigned long long)s.generatedChunks,
        (unsigned long long)s.failedGeneratedChunks,
        (unsigned long long)s.framesSent,
        (unsigned long long)s.framesReceived
    );
    return buf;
}

} // namespace dh::bedrock
