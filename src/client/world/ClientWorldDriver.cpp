#include "client/world/ClientWorldDriver.h"

#include "bedrock_common/BdsWorldGenerator.h"
#include "bedrock_common/BedrockLevelInfo.h"
#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "bedrock_common/KvChunkHashStore.h"
#include "bedrock_common/LevelDbLodStore.h"
#include "client/hud/DebugHud.h"
#include "client/render/LodRenderer.h"
#include "client/render/RenderHooks.h"
#include "client/render/TestFeed.h"
#include "client/world/ClientDriverImpl.h"

#include "dh/net/Messages.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/network/GameConnectionInfo.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/LevelData.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace dh::bedrock {

std::atomic<ClientWorldDriver::Impl*> gIngestTarget{nullptr};
std::atomic<int>                      gBusy{0};
std::atomic<bool>                     gDrainInFlight{false};
std::atomic<std::int64_t>             gDrainQueuedMs{0};
std::atomic<std::uint64_t>            gDrains{0};

namespace {

using namespace std::chrono_literals;

std::string slurp(const std::filesystem::path& p) {
    std::ifstream     in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}

std::filesystem::path levelDir(Session& s, dh::ILevelHandle& h) {
    // "sp_<name>_<level id>": the level id (world folder) keeps two worlds with the same display name apart.
    // "mp_<address>_<port>" (fallback "mp_<level name>_<level id>"): one folder per server, like Java's per-server
    // folder; the prefixes keep the two kinds from ever colliding. worldName already carries the "mp_" prefix.
    return s.paths.worldDataDir(s.singleplayer ? "sp_" + s.worldName : s.worldName)
         / sanitizePathComponent(std::string(h.dhIdentifier()));
}

/// "mp_<host>_<port>" of the server this client is connected to; falls back to "mp_<level name>_<level id>" when the
/// connection info has no address (Realms / friend join / LAN signaling, or the lookup failed).
std::string remoteWorldName(ClientInstance& ci, ::Level& level, std::string& how) {
    try {
        if (auto info = ci.getGameConnectionInfo()) {
            // The address as typed (hostname) first: stable when the server's IP changes or DNS has several records.
            // GameConnectionInfo's SDK layout has no destructor (TypedStorage members), so its strings leak once per
            // session start; harmless, and the object is never copied or moved.
            std::string host = info->mUnresolvedUrl.get();
            if (host.empty()) host = info->mHostIpAddress.get();
            if (!host.empty()) {
                const int port = info->mPort;
                how            = "connection info (host '" + host + "', port " + std::to_string(port) + ")";
                return "mp_" + sanitizePathComponent(host) + "_" + std::to_string(port);
            }
        }
    } catch (...) {
        DH_LOG_W("client driver: getGameConnectionInfo threw, using the level identity");
    }
    std::string name = level.getLevelData().mLevelName.get();
    std::string id   = level.getLevelId();
    how              = "level identity (name '" + name + "', id '" + id + "')";
    return "mp_" + sanitizePathComponent(name.empty() ? std::string("world") : name) + "_" + sanitizePathComponent(id);
}

/// Waits (bounded) until no pool executor runs or holds a task. Per-level close uses this instead of shutting the
/// pools down: the pools outlive the level (dimension change) and are only shut down by stopSession().
void waitPoolsIdle(dh::ThreadPools& p) {
    dh::Executor* all[] = {
        &p.fileHandler(),
        &p.renderLoader(),
        &p.lodBuilder(),
        &p.updatePropagator(),
        &p.worldGen(),
        &p.networkCompression()
    };
    const auto deadline = SteadyClock::now() + 5s;
    for (;;) {
        bool idle = true;
        for (auto* e : all)
            if (e->queueSize() != 0 || e->running() != 0) idle = false;
        if (idle || SteadyClock::now() > deadline) return;
        std::this_thread::sleep_for(2ms);
    }
}

} // namespace

//=========//
// Session //
//=========//

ClientLevelHandle* Session::clientHandle(int dim) {
    std::lock_guard lock(handlesMutex);
    auto            it = handles.find(dim);
    return it == handles.end() ? nullptr : it->second.client.get();
}

ClientLevelHandle* Session::serverHandle(int dim) {
    std::lock_guard lock(handlesMutex);
    auto            it = handles.find(dim);
    return it == handles.end() ? nullptr : it->second.server.get();
}

Dimension* Session::findDimension(::Level& level, int dimId) {
    Dimension* found = nullptr;
    level.forEachDimension([&](Dimension& d) -> bool {
        if (d.mId->mValue == dimId) {
            found = &d;
            return false;
        }
        return true;
    });
    return found;
}

//=======//
// Impl  //
//=======//

bool ClientWorldDriver::Impl::loadTable() {
    if (table) return true;
    if (tableFailed) return false;
    auto mod = ll::mod::NativeMod::current();
    if (!mod) return false;
    const auto dir = mod->getModDir() / "data";
    auto       r   = dh::BlockTable::load(slurp(dir / "block_table.json"), slurp(dir / "biome_table.json"));
    if (!r.has_value()) {
        tableFailed = true;
        DH_LOG_E(
            "client driver: cannot load block tables from %s: %s",
            dir.string().c_str(),
            r.error().message.c_str()
        );
        return false;
    }
    table.emplace(std::move(r.value()));
    DH_LOG_I("client driver: block table loaded (%zu blocks, %zu biomes)", table->blockCount(), table->biomeCount());
    return true;
}

void ClientWorldDriver::Impl::install() {
    if (installed) return;
    installed  = true;
    auto paths = ModPaths::fromCurrentMod();
    ConfigBridge::instance().init(paths.configDir);

    auto& bus    = ll::event::EventBus::getInstance();
    joinListener = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([this](auto&) {
        try {
            DH_LOG_I("level: join");
            nextStartAttemptMs.store(0);
        } catch (...) {}
    });
    exitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([this](auto&) {
        try {
            DH_LOG_I("level: exit");
            stopSession("level exit");
        } catch (...) {
            DH_LOG_E("client driver: exception swallowed in exit event");
        }
    });
    tickListener = bus.emplaceListener<ll::event::ClientLevelTickEvent>([this](auto&) {
        lastEventTickMs.store(nowMs());
        onClientTick();
    });
    installHooks();
    // Java: client.enableRendering is the master switch; the pipeline (world driver + renderer) starts with it
    // and the session begins on the first tick with a local player. client.keys.toggleRendering (if bound) still toggles it.
    const bool autoOn = ConfigBridge::instance().config().options().client.enableRendering;
    DH_LOG_I(
        "client driver installed (pipeline %s by client.enableRendering; client.keys.toggleRendering on/off, debug Numpad - ingestion, Shift+F5 test feed only with DH_TEST_FEED=1)",
        autoOn ? "on" : "off"
    );
    if (autoOn) setPipeline(true);
}

void ClientWorldDriver::Impl::uninstall() {
    if (!installed) return;
    installed = false;
    pipeline.store(false);
    stopSession("uninstall");
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* l : {&joinListener, &exitListener, &tickListener, &blockListener}) {
        if (*l) bus.removeListener(*l);
        l->reset();
    }
    uninstallHooks();
}

void ClientWorldDriver::Impl::setPipeline(bool on) {
    if (pipeline.exchange(on) == on) return;
    DH_LOG_I("client driver: pipeline %s", on ? "on" : "off");
    render::setEnabled(on);
    if (on) {
        nextStartAttemptMs.store(0);
        dh::hud::set("pipeline", "on");
        // a session starts from the next game tick (needs a local player)
    } else {
        stopSession("pipeline off");
        dh::hud::set("pipeline", "off");
    }
}

//-----------//
// session   //
//-----------//

bool ClientWorldDriver::Impl::startSession() {
    auto ci = ll::service::getClientInstance();
    if (!ci) return false;
    LocalPlayer* lp = ci->getLocalPlayer();
    if (!lp) return false;
    ClientLevel* cl = ci->getLevel();
    if (!cl) return false;
    if (!loadTable()) return false;

    auto s         = std::make_unique<Session>();
    s->paths       = ModPaths::fromCurrentMod();
    s->config      = &ConfigBridge::instance().config();
    s->clientLevel = cl;

    // --- singleplayer or remote? --------------------------------------------------------------
    // ll::service::getLevel() is the SERVER-targeted accessor (Bedrock.h); in singleplayer it is expected to return
    // the integrated server's Level, when remote it should be null. getMultiPlayerLevel() / ci->getLevel() are the
    // client Level. All candidates are logged so the first in-game run settles the open question (checklist).
    auto     serverCandidate = ll::service::getLevel();
    auto     clientCandidate = ll::service::getMultiPlayerLevel();
    ::Level* server          = nullptr;
    DH_LOG_I(
        "client driver: levels: ci->getLevel()=%p (isClientSide=%d) getMultiPlayerLevel()=%p getLevel()=%p "
        "(isClientSide=%d)",
        static_cast<void*>(cl),
        cl->isClientSide() ? 1 : 0,
        static_cast<void*>(clientCandidate.as_ptr()),
        static_cast<void*>(serverCandidate.as_ptr()),
        serverCandidate ? (serverCandidate->isClientSide() ? 1 : 0) : -1
    );
    if (serverCandidate && !serverCandidate->isClientSide() && serverCandidate.as_ptr() != static_cast<::Level*>(cl))
        server = serverCandidate.as_ptr();
    if (!server) {
        if (auto mc = ll::service::getMinecraft(false)) {
            ::Level* l = mc->getLevel();
            DH_LOG_I("client driver: getMinecraft(false)->getLevel()=%p", static_cast<void*>(l));
            if (l && !l->isClientSide() && l != static_cast<::Level*>(cl)) server = l;
        }
    }
    s->serverLevel  = server;
    s->singleplayer = server != nullptr;
    s->ingestLevel  = s->singleplayer ? server : static_cast<::Level*>(cl);
    s->worldName    = "world";
    if (s->singleplayer) {
        std::string name = server->getLevelData().mLevelName.get();
        if (name.empty()) name = "world";
        std::string id = server->getLevelId();
        s->worldName   = id.empty() ? name : name + "_" + id;
    } else {
        std::string how;
        s->worldName = remoteWorldName(*ci, *cl, how);
        DH_LOG_I("client driver: remote world folder '%s' from %s", s->worldName.c_str(), how.c_str());
    }

    // --- pools, serializer, transport ---------------------------------------------------------
    const dh::Options& opts = s->config->options();
    dh::ThreadOptions  to;
    to.numberOfThreads = opts.common.multiThreading.numberOfThreads;
    to.runTimeRatio    = static_cast<float>(opts.common.multiThreading.threadRunTimeRatio);
    s->pools           = std::make_unique<dh::ThreadPools>(to);
    s->serializer      = std::make_unique<BlockSerializer>(*table);
    s->worker          = std::make_unique<dh::SingleThreadExecutor>("DH Client Driver");

    Session*       raw  = s.get();
    dh::WorldDeps& d    = s->deps;
    d.clock             = &s->clock;
    d.registry          = &*table;
    d.options           = &s->config->options();
    d.exec              = dh::LevelExecutors::fromThreadPools(*s->pools);
    d.tickServerNetwork = false;
    d.storeFactory      = [raw](dh::ILevelHandle& h) -> std::unique_ptr<dh::ILodStore> {
        const auto      dir = levelDir(*raw, h);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto store = LevelDbLodStore::open(dir / "lods");
        if (!store) {
            DH_LOG_E(
                "client driver: cannot open LOD store %s: %s",
                dir.string().c_str(),
                store.error().message.c_str()
            );
            return nullptr;
        }
        std::size_t n = 0;
        if (auto all = (*store)->allPositions(); all.has_value()) n = all.value().size();
        DH_LOG_I("store: %zu sections loaded (%s, %s)", n, std::string(h.dhIdentifier()).c_str(), dir.string().c_str());
        return std::unique_ptr<dh::ILodStore>(std::move(*store));
    };
    d.chunkHashFactory = [raw](dh::ILevelHandle& h) -> std::unique_ptr<dh::IChunkHashStore> {
        const auto dir = levelDir(*raw, h);
        auto       kv  = LeviKeyValue::open(dir / "chunkhash");
        if (!kv) {
            DH_LOG_E(
                "client driver: cannot open chunk hash DB %s: %s",
                dir.string().c_str(),
                kv.error().message.c_str()
            );
            return nullptr;
        }
        return std::make_unique<KvChunkHashStore>(std::move(*kv));
    };
    if (s->singleplayer) {
        d.chunkGeneratorFactory = [raw](dh::ILevelHandle& h) -> std::unique_ptr<dh::IChunkGenerator> {
            // Only the SERVER handle reaches the server module (World.cpp), and it wraps the server Dimension.
            auto& ch = static_cast<ClientLevelHandle&>(h);
            if (!ch.dimension()) return nullptr;
            return std::make_unique<BdsWorldGenerator>(
                *ch.dimension(),
                *raw->serializer,
                raw->pools->worldGen(),
                raw->config->options().common.worldGenerator.scratchThreads
            );
        };
    }
    d.sinkFactory    = [](dh::ILevelHandle&) -> dh::IVertexSink* { return &render::renderer().sink(); };
    d.playerBlockPos = [raw]() -> std::optional<dh::BlockPos2D> {
        if (!raw->havePlayer.load()) return std::nullopt;
        return dh::BlockPos2D{raw->playerX.load(), raw->playerZ.load()};
    };
    d.activeClientLevel = [raw]() -> dh::ILevelHandle* {
        const int dim = raw->desiredDim.load();
        return dim == kNoDimension ? nullptr : raw->clientHandle(dim);
    };
    d.drainExecutors = [raw] { waitPoolsIdle(*raw->pools); };

    if (!s->singleplayer) {
        s->transport      = std::make_unique<PacketTransport>();
        d.clientTransport = s->transport.get();
    }

    if (s->singleplayer) s->world = std::make_unique<dh::DhClientServerWorld>(d);
    else s->world = std::make_unique<dh::DhClientWorld>(d);
    s->ticker = std::make_unique<dh::WorldTicker>(*s->world, s->clock, dh::kWorldTickRateMs);

    if (!s->singleplayer) {
        if (auto* net = static_cast<dh::DhClientWorld*>(s->world.get())->networkState()) {
            net->setLevelInitHandler([](const dh::net::LevelInitMessage& m) {
                ClientWorldDriver::instance().onLevelKey(m.dimensionResourceLocation, m.levelKey);
            });
        }
        s->transport->clientConnect();
    }

    DH_LOG_I(
        "client driver: session started: %s, world '%s', %d worker threads, store root %s",
        s->singleplayer ? "SINGLEPLAYER (DhClientServerWorld)" : "REMOTE (DhClientWorld over dh:frame; server or client-only)",
        s->worldName.c_str(),
        to.numberOfThreads,
        s->paths.dataDir.string().c_str()
    );

    {
        std::unique_lock lk(life);
        session = std::move(s);
    }
    if (ingestion.load()) gIngestTarget.store(this);
    dh::hud::set("mode", session->singleplayer ? "singleplayer" : "pending");
    if (!session->singleplayer) DH_LOG_I("client driver: mode = pending (config sent, waiting for a DH reply)");
    return true;
}

void ClientWorldDriver::Impl::stopSession(const char* why) {
    std::lock_guard gameLock(gameMutex); // not while a client tick is inside the session
    if (!session) return;
    DH_LOG_I("client driver: stopping session (%s)", why);
    Session& s = *session;
    s.stopping.store(true);

    // Every step has its own try/catch so a failure never skips the ordered steps after it.
    auto step = [](const char* name, auto&& fn) {
        try {
            fn();
        } catch (...) {
            DH_LOG_E("client driver: exception in stop step '%s'", name);
        }
    };

    // 1. ingestion hooks off, wait for threads inside a hook / drain
    step("hooks off", [&] {
        gIngestTarget.store(nullptr);
        while (gBusy.load() != 0) std::this_thread::sleep_for(1ms);
        clearPending(s);
        gDrainInFlight.store(false);
    });
    // 2. worker: finishes the step in flight (it may be loading a level), then exits
    step("worker", [&] {
        s.worker->shutdown();
        s.worker->join();
    });
    // 3. cancel world generation so a stopping integrated server cannot stall the close on the generator's hop TTL
    step("cancel generation", [&] {
        if (!s.world) return;
        for (dh::AbstractLevel* level : s.world->allLoadedLevels())
            if (level && level->isServerLevel() && level->isClientLevel())
                if (auto* gen = static_cast<dh::DhClientServerLevel*>(level)->serverside().chunkGenerator())
                    static_cast<BdsWorldGenerator*>(gen)->stopWorkersAndWait(); // only generator the factory makes
    });
    // 4. nothing renders from the world any more; close everything in the documented order
    std::unique_lock lk(life);
    step("world close", [&] {
        if (s.world) s.world->close(); // levels: modules/trees -> save flush -> drain -> provider (Level.h)
    });
    step("world destroy", [&] {
        s.ticker.reset();
        s.world.reset(); // ClientNetworkState detaches the transport listener
    });
    step("transport", [&] {
        if (s.transport) {
            s.transport->clientDisconnect();
            s.transport->shutdown();
            s.transport.reset();
        }
    });
    step("pools", [&] {
        if (s.pools) {
            s.pools->shutdown();
            s.pools->picker().joinAll();
            s.pools.reset();
        }
    });
    s.handles.clear();
    s.serializer.reset();
    session.reset();
    lk.unlock();

    step("renderer", [&] {
        render::renderer().sink().requestClear();
        render::testfeed::reset();
        dh::hud::set("mode", "none");
        dh::hud::set("world.levels", 0);
        dh::hud::set("tree.enabled", 0);
    });
    DH_LOG_I("client driver: session stopped");
}

//------//
// tick //
//------//

void ClientWorldDriver::Impl::ensureHandles(Session& s, int dim) {
    {
        std::lock_guard lock(s.handlesMutex);
        if (s.handles.contains(dim)) return;
    }
    Dimension* cd = s.findDimension(*s.clientLevel, dim);
    if (!cd) return;
    const dh::LevelInfo ci = levelInfoOf(*cd);
    if (ci.maxHeight <= ci.minHeight || (ci.maxHeight - ci.minHeight) % 16 != 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            DH_LOG_W("client driver: dimension %s has an unsupported height range", ci.dimensionName.c_str());
        return;
    }
    HandlePair hp;
    if (s.singleplayer) {
        Dimension* sd = s.findDimension(*s.serverLevel, dim);
        if (!sd) return;
        hp.server = std::make_unique<ClientLevelHandle>(*sd, dim, levelInfoOf(*sd), *table, false);
    }
    hp.client = std::make_unique<ClientLevelHandle>(*cd, dim, ci, *table, true);
    if (hp.server) hp.client->setPaired(hp.server.get());
    DH_LOG_I(
        "client driver: handles for dimension %s (id %d, %d..%d)",
        ci.dimensionName.c_str(),
        dim,
        ci.minHeight,
        ci.maxHeight
    );
    std::lock_guard lock(s.handlesMutex);
    s.handles.emplace(dim, std::move(hp));
}

void ClientWorldDriver::Impl::onClientTick() {
    if (!pipeline.load()) return;
    std::unique_lock gl(gameMutex, std::try_to_lock);
    if (!gl) return;
    try {
        if (!session) {
            if (nowMs() < nextStartAttemptMs.load()) return;
            if (!startSession()) {
                nextStartAttemptMs.store(nowMs() + 2000); // no local player yet / failure: retry quietly
                return;
            }
        }
        Session& s  = *session;
        auto     ci = ll::service::getClientInstance();
        if (!ci || s.stopping.load()) return;
        LocalPlayer* lp = ci->getLocalPlayer();
        if (!lp) {
            s.havePlayer.store(false);
            return;
        }
        const auto& pos = lp->getPosition();
        s.playerX.store(static_cast<std::int32_t>(std::floor(pos.x)));
        s.playerY.store(static_cast<std::int32_t>(std::floor(pos.y)));
        s.playerZ.store(static_cast<std::int32_t>(std::floor(pos.z)));
        s.havePlayer.store(true);
        const int dim = static_cast<int>(lp->getDimensionId());
        ensureHandles(s, dim);
        s.desiredDim.store(dim);

        if (s.transport) (void)s.transport->flushOutbound();
        pollMode(s);
        if (ingestion.load()) scheduleIngest(s);

        if (!s.tickInFlight.exchange(true)) {
            const bool queued = s.worker->execute([this, &s] {
                try {
                    workerStep(s);
                } catch (...) {
                    DH_LOG_E("client driver: exception swallowed in worker step");
                }
                s.tickInFlight.store(false);
            });
            if (!queued) s.tickInFlight.store(false);
        }
        updateHud(s);
    } catch (...) {
        static int n = 0;
        if (n++ < 5) DH_LOG_E("client driver: exception swallowed in client tick");
    }
}

void ClientWorldDriver::Impl::pollMode(Session& s) {
    if (s.singleplayer || !s.world) return;
    auto* net = static_cast<dh::DhClientWorld*>(s.world.get())->networkState();
    if (!net) return;
    const dh::mp::DhMode m = net->dhMode();
    if (static_cast<int>(m) != s.loggedMode) {
        const int prev = s.loggedMode;
        s.loggedMode   = static_cast<int>(m);
        const auto ms  = static_cast<long long>(net->modeDecisionMs());
        if (m == dh::mp::DhMode::Server) {
            DH_LOG_I(
                "client driver: mode = server (%s after %lld ms%s)",
                net->modeReason().c_str(),
                ms,
                prev == static_cast<int>(dh::mp::DhMode::ClientOnly)
                    ? "; LATE reply after the client-only fallback: levels re-key to the server's LevelInit key"
                    : ""
            );
            dh::hud::set("mode", "server");
        } else if (m == dh::mp::DhMode::ClientOnly) {
            DH_LOG_I(
                "client driver: mode = client-only (no DH reply within %lld ms)",
                static_cast<long long>(dh::mp::ClientNetworkState::kServerReplyTimeoutMs)
            );
            dh::hud::set("mode", "client-only");
        }
    }
}

void ClientWorldDriver::Impl::onFrameFallback() {
    if (!pipeline.load()) return;
    if (nowMs() - lastEventTickMs.load() < 250) return; // the tick event is alive
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) DH_LOG_W("client driver: ClientLevelTickEvent not seen, ticking from the render hook");
    onClientTick();
}

void ClientWorldDriver::Impl::workerStep(Session& s) {
    if (s.stopping.load()) return;
    const auto t0  = SteadyClock::now();
    const int  dim = s.desiredDim.load();

    // dimension change: unload the old level first (its sections must leave the shared uploader)
    int loaded = s.loadedDim.load();
    if (loaded != kNoDimension && loaded != dim) {
        unloadLevels(s, loaded);
        loaded = kNoDimension;
    }
    // a late LevelInit key (reply after the client-only fallback / after the 1 s load delay): reload under that key
    // (Java ClientPluginChannelApi.onLevelInitMessage loads the keyed level next to the unkeyed one)
    // The handle itself is asked (not only rekeyDim): two dimensions can park keys before the worker runs, and a
    // single atomic would forget one of them.
    if (loaded != kNoDimension && (s.rekeyDim.load() == loaded || [&] {
            auto* c = s.clientHandle(loaded);
            return c && c->hasPendingKey();
        }())) {
        s.rekeyDim.store(kNoDimension);
        unloadLevels(s, loaded);
        if (auto* c = s.clientHandle(loaded); c && c->applyPendingKey())
            DH_LOG_I("net: level of dimension %d re-keyed to '%s'", loaded, std::string(c->dhIdentifier()).c_str());
        loaded = kNoDimension;
    }
    if (loaded == kNoDimension && dim != kNoDimension && s.havePlayer.load()) {
        if (loadLevels(s, dim)) loaded = dim;
    }
    if (loaded != kNoDimension) s.ticker->poll();

    const double ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
    s.tickMs.store(s.tickMs.load() * 0.9 + ms * 0.1);
}

bool ClientWorldDriver::Impl::loadLevels(Session& s, int dim) {
    ClientLevelHandle* c = s.clientHandle(dim);
    if (!c) return false;
    ClientLevelHandle* sv = s.singleplayer ? s.serverHandle(dim) : nullptr;
    if (s.singleplayer && !sv) return false;
    ++s.loadAttempts;

    if (sv && !s.world->getOrLoadLevel(*sv)) return false;
    dh::AbstractLevel* level = s.world->getOrLoadLevel(*c);
    if (!level) {
        if (sv) s.world->unloadLevel(*sv);
        return false;
    }
    c->lockIdentity();
    if (sv) sv->lockIdentity();
    s.activeMinY.store(c->info().minHeight());
    s.activeMaxY.store(c->info().maxHeight());
    s.loadedDim.store(dim);
    requestRescan(s); // chunks that were already loaded before the level existed
    DH_LOG_I("client driver: level [%s] loaded (dimension %d)", std::string(c->dhIdentifier()).c_str(), dim);
    return true;
}

void ClientWorldDriver::Impl::unloadLevels(Session& s, int dim) {
    std::unique_lock lk(life); // the render thread must not be inside the level being closed
    s.loadedDim.store(kNoDimension);
    ClientLevelHandle* c  = s.clientHandle(dim);
    ClientLevelHandle* sv = s.serverHandle(dim);
    if (c) s.world->unloadLevel(*c);
    if (sv) s.world->unloadLevel(*sv);
    {
        std::lock_guard lock(s.pendingMutex); // keep chunks noted for the NEW dimension
        for (auto it = s.pending.begin(); it != s.pending.end();)
            it = it->first.dim == dim ? s.pending.erase(it) : std::next(it);
    }
    DH_LOG_I("client driver: level of dimension %d unloaded", dim);
}

void ClientWorldDriver::Impl::updateHud(Session& s) {
    const auto now = SteadyClock::now();
    if (now - s.lastHud < 500ms) return;
    s.lastHud = now;

    dh::hud::set("world.levels", s.world->loadedLevelCount());
    // tree.enabled = sections the tick enabled; tree.culled = core frustum rejects; tree.notReady = enabled but no
    // GPU buffers yet. render.listed = enabled - culled - notReady.
    dh::hud::set("tree.enabled", s.lastEnabled.load());
    dh::hud::set("tree.culled", s.lastCulled.load());
    dh::hud::set("tree.notReady", s.lastNotReady.load());
    // render.darkSections = enabled sections whose render data is sky 0 / block 0 on every top datapoint. >0: the DATA
    // is dark (look at the generator/updater); 0 while the screen is black: the darkness is GPU-side
    // (shader/fog/blend).
    dh::hud::set("render.darkSections", s.lastDark.load());
    dh::hud::set("tick.ms", s.tickMs.load());
    dh::hud::set("ingest.chunks", static_cast<std::int64_t>(s.ingested.load()));
    dh::hud::set("ingest.skipped", static_cast<std::int64_t>(s.skipped.load() + s.unchanged.load()));
    dh::hud::set("ingest.on", ingestion.load() ? "on" : "off");
    dh::hud::set("ingest.drains", static_cast<std::int64_t>(gDrains.load()));
    {
        const std::int64_t       q = gDrainQueuedMs.load();
        static std::atomic<bool> warned{false};
        if (q != 0 && nowMs() - q > 2000 && !warned.exchange(true))
            DH_LOG_W(
                "client ingest: a chunk drain has been queued on the server thread for >2 s without running "
                "(ServerThreadExecutor not ticking in this DLL?)"
            );
    }
    if (s.transport) {
        dh::hud::set("net.rx", static_cast<std::int64_t>(s.transport->framesReceived()));
        dh::hud::set("net.tx", static_cast<std::int64_t>(s.transport->framesSent()));
        if (auto* net = static_cast<dh::DhClientWorld*>(s.world.get())->networkState())
            dh::hud::set("net.sections", static_cast<std::int64_t>(net->sectionsReceived()));
    } else {
        dh::hud::set("net.rx", 0);
        dh::hud::set("net.tx", 0);
        dh::hud::set("net.sections", 0);
    }

    std::shared_lock lk(life, std::try_to_lock);
    if (!lk) return;
    const int loaded = s.loadedDim.load();
    if (loaded == kNoDimension) {
        dh::hud::set("tree.loading", 0);
        dh::hud::set("gen.queued", 0);
        return;
    }
    ClientLevelHandle* c = s.clientHandle(loaded);
    if (!c) return;
    dh::AbstractLevel* level = s.world->getLevel(*c);
    if (!level) return;
    dh::ClientLevelModule& module = s.singleplayer ? static_cast<dh::DhClientServerLevel*>(level)->clientside()
                                                   : static_cast<dh::DhClientLevel*>(level)->clientside();
    if (auto tree = module.tree()) {
        dh::hud::set("tree.loading", static_cast<std::int64_t>(tree->missingRetrievalCount()));
        dh::hud::set("gen.queued", static_cast<std::int64_t>(tree->queuedRetrievalCount()));
    }
}

//=============//
// public API  //
//=============//

ClientWorldDriver::ClientWorldDriver() : impl_(std::make_unique<Impl>()) {}
ClientWorldDriver::~ClientWorldDriver() = default;

ClientWorldDriver& ClientWorldDriver::instance() {
    static ClientWorldDriver inst;
    return inst;
}

void ClientWorldDriver::install() { impl_->install(); }
void ClientWorldDriver::uninstall() { impl_->uninstall(); }
void ClientWorldDriver::setPipeline(bool on) { impl_->setPipeline(on); }
bool ClientWorldDriver::pipeline() const { return impl_->pipeline.load(); }

void ClientWorldDriver::setIngestion(bool on) {
    Impl& m = *impl_;
    m.ingestion.store(on);
    {
        std::lock_guard gl(m.gameMutex); // not while stopSession tears the session down
        if (m.session && !m.session->stopping.load()) {
            if (on) {
                gDrainInFlight.store(false);
                m.requestRescan(*m.session); // chunks loaded while ingestion was off
                gIngestTarget.store(&m);
            } else {
                gIngestTarget.store(nullptr);
            }
        }
    }
    DH_LOG_I("client driver: chunk ingestion %s (debug Numpad -)", on ? "on" : "off");
    dh::hud::set("ingest.on", on ? "on" : "off");
}
bool ClientWorldDriver::ingestion() const { return impl_->ingestion.load(); }

void ClientWorldDriver::onFrame() {
    try {
        impl_->onFrameFallback();
    } catch (...) {}
}

void ClientWorldDriver::onLevelKey(const std::string& dimensionResource, const std::string& levelKey) {
    // Runs on the PacketTransport dispatch thread; the session outlives the transport (stopSession order).
    try {
        Impl& m = *impl_;
        if (!m.session) return;
        Session&        s = *m.session;
        std::lock_guard lock(s.handlesMutex);
        for (auto& [dim, hp] : s.handles) {
            if (hp.client && hp.client->dimensionResource() == dimensionResource) {
                hp.client->setLevelKey(levelKey);
                if (hp.client->hasPendingKey()) s.rekeyDim.store(dim);
                DH_LOG_I("net: LevelInit for %s -> level key '%s'", dimensionResource.c_str(), levelKey.c_str());
            }
        }
    } catch (...) {
        DH_LOG_E("client driver: exception swallowed in LevelInit handler");
    }
}

RenderListStatus
ClientWorldDriver::collectRenderList(dh::RenderListParams params, bool coreFrustum, RenderListResult& out) {
    Impl& m = *impl_;
    out.positions.clear();
    if (!m.pipeline.load()) return RenderListStatus::NoSession;
    std::shared_lock lk(m.life, std::try_to_lock);
    if (!lk) return RenderListStatus::Unavailable;
    if (!m.session) return RenderListStatus::Unavailable;
    Session& s = *m.session;
    if (s.stopping.load() || !s.world) return RenderListStatus::Unavailable;
    const int loaded = s.loadedDim.load();
    if (loaded == kNoDimension) return RenderListStatus::Unavailable;
    ClientLevelHandle* c = s.clientHandle(loaded);
    if (!c) return RenderListStatus::Unavailable;
    dh::AbstractLevel* level = s.world->getLevel(*c);
    if (!level || level->isClosed()) return RenderListStatus::Unavailable;

    dh::ClientLevelModule& module = s.singleplayer ? static_cast<dh::DhClientServerLevel*>(level)->clientside()
                                                   : static_cast<dh::DhClientLevel*>(level)->clientside();
    if (!module.isRendering()) return RenderListStatus::Unavailable;

    // Java passes minHeight and `minHeight + level.getMaxHeight()` where Java's getMaxHeight() is the world HEIGHT.
    // Our ILevelInfo::maxHeight() is the exclusive top Y already (= min + height), so it is passed unchanged: the
    // intended range, with no double add of the min (documented in the internal m1-lods spike notes).
    out.minY = c->info().minHeight();
    out.maxY = c->info().maxHeight();
    if (coreFrustum) {
        params.options = s.deps.options;
    } else { // core culling off: the renderer's own Frustum5 test is the only cull (matrix-layout fallback)
        if (s.noCullAge++ % 120 == 0) s.noCull = *s.deps.options;
        s.noCull.client.graphics.culling.disableFrustumCulling = true;
        params.options                                         = &s.noCull;
    }
    params.worldMinY = out.minY;
    params.worldMaxY = out.maxY;
    const auto& list = module.collectRenderList(params);
    out.positions.reserve(list.size());
    for (const auto& e : list) out.positions.push_back(e.pos);
    const auto st = module.lastRenderListStats();
    s.lastEnabled.store(st.enabled);
    s.lastCulled.store(st.culled);
    s.lastNotReady.store(st.notRenderable);
    s.lastDark.store(st.darkSections);
    return RenderListStatus::Ready;
}

} // namespace dh::bedrock
