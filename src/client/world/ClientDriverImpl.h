#pragma once

// Internal state of ClientWorldDriver, shared by ClientWorldDriver.cpp (lifecycle, deps, tick, render list) and
// ClientIngest.cpp (hooks, pending chunk set, snapshot drain). Not a public header.

#include "bedrock_common/BlockSerial.h"
#include "bedrock_common/PacketTransport.h"
#include "bedrock_common/Platform.h"
#include "client/world/ClientLevelHandle.h"
#include "client/world/ClientWorldDriver.h"

#include "dh/config/ConfigStore.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/IClock.h"
#include "dh/thread/SingleThreadExecutor.h"
#include "dh/thread/ThreadPools.h"
#include "dh/world/World.h"
#include "dh/world/WorldDeps.h"

#include "ll/api/event/ListenerBase.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <tuple>

class Level;
class LevelChunk;
class Dimension;

namespace dh::bedrock {

using SteadyClock = std::chrono::steady_clock;

inline constexpr int kNoDimension = -999;

struct PendingChunk {
    int                     tries = 0;
    SteadyClock::time_point due{};
};

struct PendingKey {
    int         dim = 0, x = 0, z = 0;
    friend bool operator<(const PendingKey& a, const PendingKey& b) {
        return std::tie(a.dim, a.x, a.z) < std::tie(b.dim, b.x, b.z);
    }
};

struct HandlePair {
    std::unique_ptr<ClientLevelHandle> server; ///< singleplayer only
    std::unique_ptr<ClientLevelHandle> client;
};

/// One world session (join -> exit, or F5 on -> off).
struct Session {
    // construction order == reverse destruction order (see the shutdown order in ClientWorldDriver.h)
    dh::SystemClock  clock;
    dh::ConfigStore* config = nullptr;
    ModPaths         paths;
    std::string      worldName;
    bool             singleplayer = false;

    ::Level* clientLevel = nullptr; ///< ClientInstance::getLevel()
    ::Level* serverLevel = nullptr; ///< singleplayer only
    ::Level* ingestLevel = nullptr; ///< where chunks are read: serverLevel in singleplayer, clientLevel when remote

    std::unique_ptr<BlockSerializer>          serializer;
    std::unique_ptr<dh::ThreadPools>          pools;
    std::unique_ptr<PacketTransport>          transport; ///< remote only
    dh::WorldDeps                             deps;
    std::unique_ptr<dh::AbstractWorld>        world;
    std::unique_ptr<dh::WorldTicker>          ticker;
    std::unique_ptr<dh::SingleThreadExecutor> worker;

    std::mutex                handlesMutex;
    std::map<int, HandlePair> handles; ///< dimension id -> handles (kept until the session ends)

    // player cache (written on the game thread, read by the worker)
    std::atomic<bool>         havePlayer{false};
    std::atomic<std::int32_t> playerX{0}, playerY{64}, playerZ{0};
    std::atomic<int>          desiredDim{kNoDimension}; ///< dimension of the local player
    std::atomic<int>          loadedDim{kNoDimension};  ///< dimension whose level is loaded (worker writes)
    std::atomic<bool>         tickInFlight{false};
    std::atomic<bool>         stopping{false};
    std::atomic<std::int64_t> loadAttempts{0};
    std::atomic<int>          activeMinY{-64}, activeMaxY{320};
    std::atomic<double>       tickMs{0.0};
    dh::Options               noCull; ///< render thread only (collectRenderList)
    std::uint32_t             noCullAge = 0;
    std::atomic<int>          lastEnabled{0}, lastCulled{0};

    // chunk ingestion
    std::mutex                         pendingMutex;
    std::map<PendingKey, PendingChunk> pending;
    std::atomic<bool>                  rescan{false}; ///< scan the loaded chunks around the player (owner thread)
    std::atomic<std::uint64_t>         noted{0}, ingested{0}, unchanged{0}, skipped{0}, deferred{0};

    SteadyClock::time_point lastHud  = SteadyClock::now();
    std::int64_t            hudTicks = 0;

    ClientLevelHandle* clientHandle(int dim);
    ClientLevelHandle* serverHandle(int dim);
    Dimension*         findDimension(::Level& level, int dimId);
};

struct ClientWorldDriver::Impl {
    // ---- process lifetime ---------------------------------------------------------------------
    std::atomic<bool> pipeline{false};
    std::atomic<bool> ingestion{true};
    bool              installed = false;

    std::optional<dh::BlockTable> table; ///< loaded once
    bool                          tableFailed = false;

    ll::event::ListenerPtr    joinListener, exitListener, tickListener, blockListener;
    std::atomic<std::int64_t> lastEventTickMs{0};
    std::mutex                gameMutex; ///< serialises onClientTick between the event and the render fallback

    std::atomic<std::int64_t> nextStartAttemptMs{0}; ///< steady-clock ms; written by setPipeline (render thread)

    // ---- session ------------------------------------------------------------------------------
    std::shared_mutex        life; ///< shared: render thread; exclusive: session start/stop, level unload
    std::unique_ptr<Session> session;

    // lifecycle (ClientWorldDriver.cpp)
    void install();
    void uninstall();
    void setPipeline(bool on);
    bool loadTable();
    bool startSession();
    void stopSession(const char* why);
    void onClientTick();
    void onFrameFallback();
    void workerStep(Session& s);
    void ensureHandles(Session& s, int dim);
    bool loadLevels(Session& s, int dim);
    void unloadLevels(Session& s, int dim);
    void updateHud(Session& s);

    // ingestion (ClientIngest.cpp)
    void installHooks();
    void uninstallHooks();
    void noteChunk(LevelChunk& lc, int debounceMs);
    void noteBlock(::Level& level, int dim, int blockX, int blockZ);
    void scheduleIngest(Session& s);
    void drain(Session& s);
    void clearPending(Session& s);
    void requestRescan(Session& s) { s.rescan.store(true); }
    void teardownSession(Session& s);
};

/// Hooks/events call into the active driver through these (nullptr when no session accepts chunks).
extern std::atomic<ClientWorldDriver::Impl*> gIngestTarget;
extern std::atomic<int>                      gBusy;
/// Drain bookkeeping is process-global (not per session): a queued server-thread task must be able to clear it even
/// when it finds no session.
extern std::atomic<bool>          gDrainInFlight;
extern std::atomic<std::int64_t>  gDrainQueuedMs; ///< 0 = not waiting to start
extern std::atomic<std::uint64_t> gDrains;        ///< drains actually executed

struct BusyGuard {
    BusyGuard() { ++gBusy; }
    ~BusyGuard() { --gBusy; }
    BusyGuard(const BusyGuard&)            = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
};

} // namespace dh::bedrock
