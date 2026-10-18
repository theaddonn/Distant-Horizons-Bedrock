#pragma once

// The dependency bundle the platform fills once and hands to a world (Java read most of this from singletons:
// ThreadPoolUtil, Config, MC_CLIENT, SingletonInjector). Everything is a non-owning reference/pointer unless
// stated; all of it must outlive the world.
//
// Threading model (document for the glue): nothing in dh/world starts a thread. Concurrency comes only from the
// injected executors; time-driven work (client tick 100 ms, update propagator 250 ms, request module, save-cache
// cleanup 1 s, server network 20 Hz) advances when the platform calls AbstractWorld::tick() - use WorldTicker.
//
// Deviation from the work-package text: the bundle carries a LevelExecutors (plain IExecutor pointers) instead of
// a ThreadPools&, so tests can run the whole stack on ManualExecutors; LevelExecutors::fromThreadPools() adapts
// the production ThreadPools.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "dh/config/Options.h"
#include "dh/multiplayer/ServerRequestHandler.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IChunkGenerator.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/ILodStore.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/ITransport.h"
#include "dh/platform/IVertexSink.h"
#include "dh/pos/BlockPos.h"
#include "dh/storage/ChunkHashStore.h"
#include "dh/thread/ThreadPools.h"
#include "dh/world/LevelHandle.h"

namespace dh {

/// The executors a level uses (Java ThreadPoolUtil getters). Built from ThreadPools with fromThreadPools(), or
/// filled with ManualExecutors in tests.
struct LevelExecutors {
    IExecutor* fileHandler      = nullptr; ///< store IO, provider reads, updater (required)
    IExecutor* renderLoader     = nullptr; ///< LodRenderSection builds (client)
    IExecutor* lodBuilder       = nullptr; ///< chunk -> LOD (required)
    IExecutor* updatePropagator = nullptr; ///< parent/child propagation (required)
    IExecutor* worldGen         = nullptr; ///< GenQueue / WorldGenerator work (server)
    IExecutor* queueThread      = nullptr; ///< GenQueue's queueing loop (Java "World Gen Queue" single thread)
    IExecutor* networkWork      = nullptr; ///< payload encode (server) / decode (client)
    IExecutor* retrievalQueuing = nullptr; ///< optional: tree's retrieval-queuing thread; null = inline in the tick

    /// ThreadPools mapping. `queueThread` borrows the file-handler pool (the loop only schedules, never blocks),
    /// `retrievalQueuing` is left null (inline).
    static LevelExecutors fromThreadPools(ThreadPools& pools) {
        LevelExecutors e;
        e.fileHandler      = &pools.fileHandler();
        e.renderLoader     = &pools.renderLoader();
        e.lodBuilder       = &pools.lodBuilder();
        e.updatePropagator = &pools.updatePropagator();
        e.worldGen         = &pools.worldGen();
        e.queueThread      = &pools.fileHandler();
        e.networkWork      = &pools.networkCompression();
        return e;
    }
    bool valid() const { return fileHandler && lodBuilder && updatePropagator; }
};

struct WorldDeps {
    IClock*         clock    = nullptr; ///< required
    IBlockRegistry* registry = nullptr; ///< required
    /// Read live (tree tick, generator plan, ...). When it changes call AbstractWorld::onOptionsChanged().
    Options*       options = nullptr; ///< required
    LevelExecutors exec;
    ILogger*       logger = nullptr; ///< user-facing failure messages + diagnostics; null = dh::log::get()

    /// Opens the persistent LOD store of a level. Returning nullptr is a level-load failure.
    std::function<std::unique_ptr<ILodStore>(ILevelHandle&)> storeFactory;
    /// Persistent chunk-hash table; empty = in-memory (hashes are lost on close).
    std::function<std::unique_ptr<IChunkHashStore>(ILevelHandle&)> chunkHashFactory;
    /// Server side: the platform's chunk generator (SingleplayerGenDriver / BdsWorldGenerator). Empty = this
    /// platform cannot generate; nullptr returned = level-load failure.
    std::function<std::unique_ptr<IChunkGenerator>(ILevelHandle&)> chunkGeneratorFactory;
    /// Client side: where a level's meshes go. Called once per started renderer with the CLIENT handle. The
    /// sink is platform owned and must outlive the level.
    std::function<IVertexSink*(ILevelHandle&)> sinkFactory;

    /// Client-only world: the transport to the DH server. null = no networking (levels run on local data).
    ITransport* clientTransport = nullptr;
    /// Server worlds: shared player table and frame router (platform owned). null = networking off.
    mp::ServerPlayers* serverPlayers = nullptr;
    mp::ServerNetwork* serverNetwork = nullptr;
    /// Server worlds: call serverNetwork->tick() from AbstractWorld::tick() (its 20 Hz contract).
    bool tickServerNetwork = true;

    /// MC_CLIENT.getPlayerBlockPos()/camera: nullopt = no player (Java playerExists()). Required for client
    /// worlds to render anything.
    std::function<std::optional<BlockPos2D>()> playerBlockPos;
    /// MC_CLIENT.getWrappedClientLevel(): the client level handle the player is in now. Empty = every client
    /// level counts as active.
    std::function<ILevelHandle*()> activeClientLevel;

    /// Blocks until the executors no longer run tasks that reference a closing level (PORTING.md: queued tasks
    /// capture `this`; ThreadPools::shutdown()+join in production, ManualExecutor::runAll in tests). Called by
    /// AbstractLevel::close() between "stop producing work" and "destroy". Empty = caller guarantees quiescence.
    std::function<void()> drainExecutors;

    ILogger& log() const { return logger ? *logger : dh::log::get(); }
    bool     valid() const { return clock && registry && options && exec.valid(); }
};

} // namespace dh
