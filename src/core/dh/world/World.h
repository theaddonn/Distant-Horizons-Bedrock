#pragma once

// Port of world/{AbstractDhWorld,DhClientWorld,DhClientServerWorld,DhServerWorld,AbstractDhServerWorld,
// IDhClientWorld,IDhServerWorld}.java: the lifecycle objects that own the levels.
//
//   DhClientWorld        client connected to a remote DH server: one DhClientLevel per dimension id, owns the
//                        ClientNetworkState (session negotiation, request tracking).
//   DhClientServerWorld  singleplayer: one DhClientServerLevel per SERVER level handle; client handles attach to it.
//                        The server level is unloaded only through its server handle (or world close); a client
//                        handle leaving just forgets itself and stops the renderer when the last viewer is gone.
//   DhServerWorld        dedicated server: DhServerLevel per server handle.
//
// Driving the world (Java used a Timer thread with IDhClientWorld.TICK_RATE_IN_MS = 100):
//   There is NO internal thread. The platform calls tick() from its own loop; WorldTicker turns an arbitrary frame
//   loop into "tick() at most every N ms" over IClock. tick() advances every level (client tick, request module,
//   propagator, delayed saves) and, for server worlds, the ServerNetwork (its contract is 20 Hz, so server drivers
//   use WorldTicker(world, clock, 50)). Level-internal cadences (propagator 250 ms, cache cleanup 1 s) are gated by
//   the clock, not by the call rate.
//
// Failure policy: a level that cannot be created (no store, generator factory failed, bad handle) yields nullptr
// and exactly ONE user-facing message through ILogger per level identifier until that level loads successfully
// (Java queued one chat message per attempt). Nothing throws.
//
// Deviations from Java: levels are closed sequentially (Java spawned a thread per level to speed shutdown, which would
// need a thread here); DhClientWorld closes its levels before the network state (Java closed the network first, but
// level shutdown needs the session to cancel in-flight requests cleanly); the DhApi level load/unload events, the
// ClientPluginChannelApi level-key delegation and PregenManager are not ported.

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dh/multiplayer/ClientNetworkState.h"
#include "dh/multiplayer/ServerRequestHandler.h"
#include "dh/net/SessionConfig.h"
#include "dh/world/Level.h"
#include "dh/world/LevelHandle.h"
#include "dh/world/WorldDeps.h"

namespace dh {

enum class WorldEnvironment : std::uint8_t { ClientOnly, ClientServer, ServerOnly };

/// IDhClientWorld.TICK_RATE_IN_MS
inline constexpr std::int64_t kWorldTickRateMs = 100;

/// Config.Server.* / Config.Common.WorldGenerator.* as session-config values (what a client asks for / a server
/// enforces).
net::SessionConfigValues sessionValuesFromOptions(const Options& options);
/// The server's own settings for ServerPlayers (Config.Server.*).
mp::ServerSettings serverSettingsFromOptions(const Options& options);

class AbstractWorld {
public:
    virtual ~AbstractWorld();
    AbstractWorld(const AbstractWorld&)            = delete;
    AbstractWorld& operator=(const AbstractWorld&) = delete;

    WorldEnvironment environment() const { return environment_; }
    const char*      environmentName() const;

    /// Loads (or returns) the level of `handle`. nullptr on failure (one user message, see file comment) or when this
    /// kind of world does not handle that kind of handle.
    virtual AbstractLevel* getOrLoadLevel(ILevelHandle& handle) = 0;
    /// true if the handle was known. See the per-world semantics in the file comment.
    virtual bool           unloadLevel(ILevelHandle& handle) = 0;
    virtual AbstractLevel* getLevel(ILevelHandle& handle)    = 0;

    /// Distinct loaded levels in load order.
    virtual std::vector<AbstractLevel*> allLoadedLevels() = 0;
    int                                 loadedLevelCount() { return static_cast<int>(allLoadedLevels().size()); }

    /// SharedApi.applyChunkUpdate / chunk load: routes a chunk to its level (see AbstractLevel::updateChunkAsync).
    bool applyChunkUpdate(
        ILevelHandle&                        handle,
        std::shared_ptr<IChunk>              chunk,
        std::vector<std::shared_ptr<IChunk>> neighbours = {}
    );

    /// Advances all levels (and the server network). Cheap when nothing is due; no-op after close().
    void tick();
    /// Options changed (render distance, generator plan, thread counts, session values ...).
    void onOptionsChanged();
    /// Closes every level in the documented order and releases network state. Idempotent.
    void close();
    bool isClosed() const { return closed_; }

    /// "<environment> World with N levels"
    std::string debugString();

protected:
    AbstractWorld(WorldEnvironment environment, const WorldDeps& deps);

    virtual void tickImpl() = 0;
    virtual void onOptionsChangedImpl() {}
    virtual void closeImpl() = 0;

    /// Logs the single user-facing failure message for `id` (deduplicated until clearLoadFailure(id)).
    void reportLoadFailure(const char* kind, const std::string& id, const std::string& detail);
    void clearLoadFailure(const std::string& id);
    void tickServerNetwork();

    WorldDeps        deps_;
    WorldEnvironment environment_;
    std::mutex       mutex_;
    bool             closed_ = false;

private:
    std::set<std::string> reportedFailures_;
};

//================//
// DhClientWorld  //
//================//

class DhClientWorld final : public AbstractWorld {
public:
    /// Java: delay loading the first level to give the server time to answer with the level key.
    static constexpr std::int64_t kFirstLevelLoadDelayMs = 1000;

    explicit DhClientWorld(const WorldDeps& deps);
    ~DhClientWorld() override;

    /// nullptr when `deps.clientTransport` is null.
    mp::ClientNetworkState* networkState() { return net_.get(); }

    DhClientLevel*              getOrLoadLevel(ILevelHandle& handle) override;
    bool                        unloadLevel(ILevelHandle& handle) override;
    DhClientLevel*              getLevel(ILevelHandle& handle) override;
    std::vector<AbstractLevel*> allLoadedLevels() override;

protected:
    void tickImpl() override;
    void onOptionsChangedImpl() override;
    void closeImpl() override;

private:
    struct Entry {
        std::string                    id;
        ILevelHandle*                  handle = nullptr; ///< the wrapper the level was created for
        std::shared_ptr<DhClientLevel> level;
        std::set<ILevelHandle*>        wrappers;
    };

    Entry*                         find(const std::string& id);
    bool                           ensureLevelKeyWhenAvailable(ILevelHandle& handle);
    std::shared_ptr<DhClientLevel> createClientLevel(ILevelHandle& handle);
    /// Java unloadLevel; the closing level is returned so the caller closes it without the world lock.
    bool unloadLocked(ILevelHandle& handle, std::shared_ptr<DhClientLevel>& toClose);

    std::unique_ptr<mp::ClientNetworkState> net_;
    std::vector<Entry>                      entries_;
    std::set<ILevelHandle*>                 levelInitRequested_;
    std::int64_t                            allowLoadingLevelsAfterMs_ = 0;
};

//=====================//
// DhClientServerWorld //
//=====================//

class DhClientServerWorld final : public AbstractWorld {
public:
    explicit DhClientServerWorld(const WorldDeps& deps);
    ~DhClientServerWorld() override;

    DhClientServerLevel*        getOrLoadLevel(ILevelHandle& handle) override;
    bool                        unloadLevel(ILevelHandle& handle) override;
    DhClientServerLevel*        getLevel(ILevelHandle& handle) override;
    std::vector<AbstractLevel*> allLoadedLevels() override;

    /// Client handles currently attached to the level of `serverHandle` (tests / debug).
    std::size_t clientWrapperCount(ILevelHandle& serverHandle);

protected:
    void tickImpl() override;
    void onOptionsChangedImpl() override;
    void closeImpl() override;

private:
    struct Entry {
        ILevelHandle*                        serverHandle = nullptr;
        std::shared_ptr<DhClientServerLevel> level;
        std::vector<ILevelHandle*>           clientWrappers; ///< Java clientLevelWrapperSetByDhLevel
    };

    Entry* findByServer(ILevelHandle* serverHandle);
    Entry* findByClient(ILevelHandle* clientHandle);

    std::vector<Entry> entries_;
};

//===============//
// DhServerWorld //
//===============//

class DhServerWorld final : public AbstractWorld {
public:
    explicit DhServerWorld(const WorldDeps& deps);
    ~DhServerWorld() override;

    DhServerLevel*              getOrLoadLevel(ILevelHandle& handle) override;
    bool                        unloadLevel(ILevelHandle& handle) override;
    DhServerLevel*              getLevel(ILevelHandle& handle) override;
    std::vector<AbstractLevel*> allLoadedLevels() override;

    /// A player moved (platform -> ServerPlayers): world gen and the distance gates read this.
    void setPlayerPosition(SessionId session, ILevelHandle& level, BlockPos2D position);

protected:
    void tickImpl() override;
    void onOptionsChangedImpl() override;
    void closeImpl() override;

private:
    struct Entry {
        ILevelHandle*                  handle = nullptr;
        std::shared_ptr<DhServerLevel> level;
    };
    Entry* find(ILevelHandle* handle);

    std::vector<Entry> entries_;
};

//=============//
// WorldTicker //
//=============//

/// Calls world.tick() when at least `periodMs` of IClock time has passed since the previous tick (no catch-up bursts).
class WorldTicker {
public:
    WorldTicker(AbstractWorld& world, const IClock& clock, std::int64_t periodMs = kWorldTickRateMs)
    : world_(world),
      clock_(clock),
      periodNanos_(periodMs * 1'000'000) {}

    /// Returns true if the world was ticked. The first call always ticks.
    bool poll() {
        const std::int64_t now = clock_.steadyNanos();
        if (started_ && now - last_ < periodNanos_) return false;
        started_ = true;
        last_    = now;
        ++ticks_;
        world_.tick();
        return true;
    }
    std::uint64_t tickCount() const { return ticks_; }

private:
    AbstractWorld& world_;
    const IClock&  clock_;
    std::int64_t   periodNanos_;
    std::int64_t   last_    = 0;
    bool           started_ = false;
    std::uint64_t  ticks_   = 0;
};

} // namespace dh
