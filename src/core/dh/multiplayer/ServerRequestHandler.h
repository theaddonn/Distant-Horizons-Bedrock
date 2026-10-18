#pragma once

// Port of multiplayer/server/{FullDataSourceRequestHandler,DataSourceRequestGroup,ServerPlayerState,
// ServerPlayerStateManager}.java plus the server half of AbstractDhServerLevel (request gating,
// real-time updates) and NetworkSession (frame dispatch).
//
//   ServerPlayers          players by transport session, shared bandwidth limit, live server settings
//   ServerRequestHandler   one per level: sync-on-load vs generation requests, request groups
//   ServerNetwork          ITransportListener: decodes frames and routes them
//
// Shutdown order: close() invalidates callbacks by dropping a token without waiting, so quiesce the
// executors/provider (or destroy the handler from outside their callbacks) before destroying it.
// An unsupported frame (wrong protocol version or unknown message id) intentionally answers
// CloseReason + disconnect.
//
// Threading: nothing here starts a thread. Slow work (provider reads, payload encoding) goes through
// the injected `work` executor; the owner calls ServerNetwork::tick() at 20 Hz.
//
// Rate limiter tickets (SupplierBasedRateAndConcurrencyLimiter) are released exactly once on every
// exit path: success, exception response, cancel, provider failure, encode failure, transport failure,
// player leaving, handler close. Java leaked the permit on several of those paths
// (FullDataSourceRequestHandler :131-140 and the sync stage failures).
//
// Further deviations: a request for an unknown level gets RequestRejected (Java: silently ignored);
// sync-on-load requests can be cancelled; a generation that finishes with Failed/Rejected/Split fails
// the waiting group (Java: waited forever); a group that is still not fully generated after
// 10 s of retries is rejected (Java: retried without bound).

#include "dh/config/Enums.h"
#include "dh/gen/GeneratedProvider.h"
#include "dh/multiplayer/Payload.h"
#include "dh/net/Messages.h"
#include "dh/net/SessionConfig.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/ITransport.h"
#include "dh/pos/BlockPos.h"
#include "dh/thread/RateLimiter.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dh::mp {

/// A group whose generation reported done but whose data is still incomplete is re-read for this long.
inline constexpr int kFulfillRetryWindowMs = 10'000;

/// The server's own (unconstrained) configuration: Config.Server.* and Config.Common.WorldGenerator.*.
/// `values` uses the session-config layout, so gating and negotiation read the same numbers.
struct ServerSettings {
    net::SessionConfigValues values;
    int                      globalBandwidthLimit = 0; ///< KB/s, 0 = unlimited
    bool                     sendLevelKeys        = true;
    std::string              serverKey; ///< already sanitised, see makeServerKey

    /// ServerPlayerState.serverKey: "" if key empty, else `serverId_trimmed(key)` with every char outside
    /// [a-zA-Z0-9-_ ] removed and spaces replaced by '_'.
    static std::string makeServerKey(int serverId, std::string_view key);
};

/// Per level facts the wrappers provide.
struct LevelInfo {
    std::string levelName;                       ///< what FullDataSourceRequest.levelName carries (getDhIdentifier)
    std::string dimension;                       ///< dimension resource location (RequestLevelInit)
    std::string levelKey;                        ///< getKeyedLevelDimensionName
    double      coordinateScale         = 1.0;   ///< dimension coordinate scale for the generation centre
    bool        internalServerGenerator = false; ///< chunkGeneratorMode == INTERNAL_SERVER
};

class ServerPlayerState {
public:
    ServerPlayerState(
        SessionId             session,
        std::string           peerName,
        ITransport&           transport,
        BandwidthLimiter&     shared,
        const ServerSettings& settings
    );

    SessionId          session() const { return session_; }
    const std::string& peerName() const { return peerName_; }

    void        setPosition(std::string levelName, BlockPos2D position);
    BlockPos2D  position() const;
    std::string levelName() const;

    void setClientConfig(const net::SessionConfigValues& values);
    /// ServerPlayerState.isReady: the client's SessionConfig arrived.
    bool isReady() const;
    /// SessionConfig.getValue: constrain(client, server); the server's own values until the client spoke.
    net::SessionConfigValues sessionConfig() const;

private:
    SessionId             session_;
    std::string           peerName_;
    const ServerSettings& settings_;

    mutable std::mutex                      mutex_;
    BlockPos2D                              position_;
    std::string                             levelName_;
    std::optional<net::SessionConfigValues> clientConfig_;

public:
    PayloadSender sender; ///< declared last: its bandwidth supplier reads the members above
};

class ServerPlayers {
public:
    ServerPlayers(ITransport& transport, const IClock& clock, ServerSettings settings = {});

    ITransport&       transport() { return transport_; }
    const IClock&     clock() const { return clock_; }
    ServerSettings    settings; ///< mutate on the tick thread only
    BandwidthLimiter& bandwidth() { return bandwidth_; }
    /// Process-wide like Java's static FullDataPayload.lastBufferId: ids stay unique across levels.
    BufferIdAllocator& bufferIds() { return bufferIds_; }

    std::shared_ptr<ServerPlayerState> add(SessionId session, std::string peerName);
    void                               remove(SessionId session);
    std::shared_ptr<ServerPlayerState> find(SessionId session) const;
    /// Ascending session id.
    std::vector<std::shared_ptr<ServerPlayerState>> all() const;
    std::vector<std::shared_ptr<ServerPlayerState>> readyPlayers() const;

    void setPosition(SessionId session, std::string levelName, BlockPos2D position);

    /// Ticks every player's PayloadSender (ascending session id).
    void tickSenders();

    using LeaveListener = std::function<void(SessionId)>;
    std::uint64_t addLeaveListener(LeaveListener l);
    void          removeLeaveListener(std::uint64_t id);

private:
    ITransport&       transport_;
    const IClock&     clock_;
    BandwidthLimiter  bandwidth_;
    BufferIdAllocator bufferIds_;

    mutable std::mutex                                      mutex_;
    std::map<SessionId, std::shared_ptr<ServerPlayerState>> players_;
    std::map<std::uint64_t, LeaveListener>                  leaveListeners_;
    std::uint64_t                                           nextListener_ = 1;
};

struct ServerHandlerStats {
    std::uint64_t payloadsEncoded     = 0;
    std::uint64_t responsesSent       = 0;
    std::uint64_t exceptionsSent      = 0;
    std::uint64_t realtimeUpdatesSent = 0;
};

class ServerRequestHandler {
public:
    using BeaconSupplier = std::function<std::vector<net::BeaconBeam>(std::uint64_t pos)>;

    ServerRequestHandler(
        ServerPlayers&     players,
        GeneratedProvider& provider,
        IExecutor&         work,
        LevelInfo          level,
        BeaconSupplier     beacons = {},
        CodecOptions       codec   = {}
    );
    ~ServerRequestHandler();
    ServerRequestHandler(const ServerRequestHandler&)            = delete;
    ServerRequestHandler& operator=(const ServerRequestHandler&) = delete;

    const LevelInfo& level() const { return level_; }

    /// AbstractDhServerLevel's FullDataSourceRequest handler: gates, then sync-on-load (clientTimestamp
    /// present) or generation (absent). Responses/exceptions go back through the transport.
    void onRequest(SessionId session, const net::FullDataSourceRequestMessage& request);

    /// CancelMessage handler. `wireFutureId` is the low 32 bits the client sent; the session is forced to the caller's.
    void cancel(SessionId session, std::uint64_t futureId);

    /// Java onWorldGenTaskComplete: the world generator finished `pos`.
    void onWorldGenTaskComplete(std::uint64_t pos);

    /// Sends finished request groups (Java: the 20 ms ticker thread).
    void tick();

    /// A player left: drops their requests (releasing tickets) and limiters.
    void onPlayerLeft(SessionId session);

    void close();

    /// The session config the client is actually held to (constrained).
    net::SessionConfigValues configMessageFor(const ServerPlayerState& player) const;

    // introspection (tests, debug menu)
    ServerHandlerStats stats() const;
    std::size_t        groupCount() const;
    /// Tickets handed out and not yet released (generation + sync, all players).
    int activeTickets() const { return activeTickets_.load(); }

private:
    struct Limiters;
    struct Request;
    struct Group;

    // request entry
    void queueWorldGen(const std::shared_ptr<ServerPlayerState>& player, std::uint64_t futureId, std::uint64_t pos);
    void queueSync(
        const std::shared_ptr<ServerPlayerState>& player,
        std::uint64_t                             futureId,
        std::uint64_t                             pos,
        std::int64_t                              clientTimestamp
    );
    void enqueueRequest(const std::shared_ptr<Request>& request);

    // groups
    void tryFulfill(const std::shared_ptr<Group>& group);
    void onGroupSourceLoaded(const std::shared_ptr<Group>& group, std::shared_ptr<const FullDataSource> source);
    void failGroup(const std::shared_ptr<Group>& group, net::RemoteException kind, const std::string& message);
    void sendGroup(const std::shared_ptr<Group>& group, const std::shared_ptr<const FullDataSource>& source);
    void deliver(const std::shared_ptr<Request>& request, const std::shared_ptr<const FullDataPayload>& payload);

    // sync
    void runSync(const std::shared_ptr<Request>& request, std::int64_t clientTimestamp);

    // real-time updates
    void onSectionUpdated(std::shared_ptr<const FullDataSource> delta);
    int  constrainedGenLimit(SessionId session) const;
    int  constrainedSyncLimit(SessionId session) const;

    // helpers
    std::shared_ptr<Limiters> limitersFor(SessionId session);
    void                      release(Request& request);
    void
    sendException(SessionId session, std::uint64_t futureId, net::RemoteException kind, const std::string& message);
    void sendResponse(SessionId session, std::uint64_t futureId, const std::shared_ptr<const FullDataPayload>& payload);
    bool sendMessage(SessionId session, const net::Message& message);
    std::vector<net::BeaconBeam> beaconsFor(std::uint64_t pos) const;
    std::int32_t                 distanceInChunks(std::uint64_t pos, const ServerPlayerState& player) const;
    void
    rejectAll(std::vector<std::shared_ptr<Request>> requests, net::RemoteException kind, const std::string& message);

    ServerPlayers&     players_;
    GeneratedProvider& provider_;
    IExecutor&         work_;
    LevelInfo          level_;
    BeaconSupplier     beacons_;
    CodecOptions       codec_;

    std::shared_ptr<bool> alive_          = std::make_shared<bool>(true);
    ListenerId            updateListener_ = 0;
    std::uint64_t         leaveListener_  = 0;
    std::atomic<bool>     closed_{false};
    std::atomic<int>      activeTickets_{0};

    mutable std::mutex                                mutex_;
    std::map<std::uint64_t, std::shared_ptr<Group>>   groupsByPos_;
    std::map<std::uint64_t, std::shared_ptr<Group>>   groupsByFutureId_;
    std::map<std::uint64_t, std::shared_ptr<Request>> syncRequests_;
    std::map<SessionId, std::shared_ptr<Limiters>>    limiters_;

    mutable std::mutex stats_mutex_;
    ServerHandlerStats stats_;
};

/// ITransportListener for the server side: frame decode + routing to players and level handlers.
class ServerNetwork final : public ITransportListener {
public:
    explicit ServerNetwork(ServerPlayers& players);
    ~ServerNetwork() override;

    void addLevel(ServerRequestHandler& handler);
    void removeLevel(ServerRequestHandler& handler);

    /// Re-sends the constrained SessionConfig to every ready player. Call after mutating ServerPlayers::settings
    /// (Java ServerPlayerState.configAnyChangeListener).
    void broadcastConfig();

    /// handlers' tick(), then every player's sender tick. Call at 20 Hz.
    void tick();

    // ITransportListener
    void onConnected(SessionId session, std::string_view peerName) override;
    void onFrame(SessionId session, std::span<const std::byte> frame) override;
    void onDisconnected(SessionId session) override;

    std::uint64_t corruptFrames() const { return corrupt_.load(); }

private:
    void                  sendConfig(const ServerPlayerState& player);
    ServerRequestHandler* findLevelByName(const std::string& name) const;
    ServerRequestHandler* findLevelByDimension(const std::string& dimension) const;

    ServerPlayers& players_;

    mutable std::mutex                 mutex_;
    std::vector<ServerRequestHandler*> levels_;
    std::atomic<std::uint64_t>         corrupt_{0};
};

} // namespace dh::mp
