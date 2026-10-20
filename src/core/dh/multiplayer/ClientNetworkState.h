#pragma once

// Port of multiplayer/client/{ClientNetworkState,ClientCongestionControl}.java plus the request/response
// tracking of AbstractNetworkEventSource/NetworkSession on the client side.
//
// The client talks to exactly one server session (the transport session announced by onConnected).
// onConnected sends our SessionConfig; the server answers with the constrained config (K1 `constrain`),
// which marks the state ready. Requests are tracked by the wire part of their futureId.
//
// All callbacks run on the transport's listener thread (or whoever drives the loopback pump); heavy
// work (payload decode) is the request queues' business, they hop to their own executor.

#include "dh/multiplayer/Payload.h"
#include "dh/net/Messages.h"
#include "dh/net/SessionConfig.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/ITransport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dh::mp {

/// ClientCongestionControl: additive increase of the requested bandwidth while the link keeps up,
/// drop to (throughput - 25 KB/s, min 1 KB/s) when it does not. Evaluated once per second, on the
/// first payload chunk after the interval elapsed.
class ClientCongestionControl {
public:
    static constexpr double       kAdditiveIncrease = 50000; // bytes per second
    static constexpr std::int64_t kIntervalMs       = 1000;

    ClientCongestionControl(const IClock& clock, std::function<void()> rateUpdateHandler);

    void reset();
    /// ClientCongestionControl.onPayloadReceived: checks the interval BEFORE counting the chunk.
    void onPayloadReceived(std::size_t chunkBytes);
    /// KB/s (desiredRate / 1000, truncated).
    int getDesiredRate() const;

private:
    const IClock&         clock_;
    std::function<void()> rateUpdateHandler_;

    mutable std::mutex mutex_;
    std::uint64_t      bytesReceived_ = 0;
    double             desiredRate_   = kAdditiveIncrease;
    std::int64_t       lastAdjustMs_  = 0;
};

enum class ServerSupport : std::uint8_t { None, LevelsOnly, Full };

/// What the connected server turned out to be (decided by the client, never negotiated).
///   Pending     we sent our config and wait for any DH frame (SessionConfig or LevelInit).
///   Server      the server answered: requests, sync-on-load and level keys are in play.
///   ClientOnly  no answer within kServerReplyTimeoutMs: the server has no DH. Levels load with a locally derived key,
///               no request traffic. A later answer (late reply) switches to Server again: Java has no latch either,
///               ClientPluginChannelApi simply handles LevelInit/SessionConfig whenever they arrive.
enum class DhMode : std::uint8_t { Pending, Server, ClientOnly };

/// What a tracked request resolves to.
struct NetResponse {
    enum class Kind : std::uint8_t {
        Payload, ///< FullDataSourceResponse with a payload (data follows in the receiver's buffer)
        Empty,   ///< FullDataSourceResponse without payload
        Remote,  ///< ExceptionMessage
        Closed,  ///< session closed / not connected
    };
    Kind                 kind = Kind::Closed;
    net::FullDataPayload payload;
    net::RemoteException exception = net::RemoteException::RequestRejected;
    std::string          message;
};

struct ClientNetConfig {
    /// What this client asks for (Config.Server.* as seen by the client). The server answers constrain(client, server).
    net::SessionConfigValues wanted;
    /// Config.Server.enableAdaptiveTransferSpeed
    bool adaptiveTransferSpeed = false;
};

class ClientNetworkState final : public ITransportListener {
public:
    using ResponseCallback = std::function<void(const NetResponse&)>;

    ClientNetworkState(
        ITransport&     transport,
        const IClock&   clock,
        IBlockRegistry& registry,
        ClientNetConfig config = {}
    );
    ~ClientNetworkState() override;
    ClientNetworkState(const ClientNetworkState&)            = delete;
    ClientNetworkState& operator=(const ClientNetworkState&) = delete;

    // ITransportListener
    void onConnected(SessionId session, std::string_view peerName) override;
    void onFrame(SessionId session, std::span<const std::byte> frame) override;
    void onDisconnected(SessionId session) override;

    bool isConnected() const;
    /// configReceived
    bool isReady() const;
    /// The negotiated (server-constrained) values; defaults until ready.
    net::SessionConfigValues sessionConfig() const;
    std::int64_t             serverTimeOffset() const;
    ServerSupport            serverSupport() const;
    std::string              debugStatus() const;
    /// A frame with a different protocol version or an unknown id arrived.
    bool sawIncompatibleFrame() const { return incompatible_.load(); }

    /// Java has no explicit timeout; the reply window before we call the server DH-less. The level-load delay
    /// (DhClientWorld::kFirstLevelLoadDelayMs, 1 s) is shorter on purpose: levels load with the local key first.
    static constexpr std::int64_t kServerReplyTimeoutMs = 5000;
    /// Evaluates (and latches) the mode; the timeout is checked here, so poll it from a tick.
    DhMode dhMode();
    /// Milliseconds from the start of the reply window (connect) to the latest mode change.
    std::int64_t modeDecisionMs() const;
    /// What decided the mode: "session config received", "level init received", "no DH reply".
    std::string modeReason() const;

    /// Response bookkeeping for the log/HUD: `pos` and the payload byte size of a received section.
    /// Logs the first 20 sections, then every 100th.
    void          noteSectionReceived(std::uint64_t pos, std::size_t bytes);
    std::uint64_t sectionsReceived() const { return sections_.load(); }
    /// decodePayload() that also reports the encoded byte size of the payload.
    Result<FullDataSource> decodePayload(const net::FullDataPayload& payload, std::size_t* bytes);

    /// Sends our config. `blocking` clears isReady until the answer arrives. With adaptive transfer speed
    /// the playerBandwidthLimit sent is the congestion control's desired rate.
    void sendConfigMessage(bool blocking = true);
    /// Replaces what we ask for and re-sends it (Java: configAnyChangeListener).
    void setWanted(const net::SessionConfigValues& values);
    /// Partial-update payloads are decoded on this executor instead of the transport thread.
    void setWorkExecutor(IExecutor* executor);
    void sendLevelInitRequest(const std::string& dimensionResourceLocation);

    /// Sends a FullDataSourceRequest. Returns its futureId (0 if the frame could not be sent, in which
    /// case `callback` has already been called with Kind::Closed).
    std::uint64_t sendRequest(
        const std::string&          levelName,
        std::uint64_t               sectionPos,
        std::optional<std::int64_t> clientTimestamp,
        ResponseCallback            callback
    );
    /// Forgets the request (a late response is ignored) and tells the server to stop (CancelMessage).
    void        cancelRequest(std::uint64_t futureId);
    std::size_t pendingRequestCount() const;

    PayloadReceiver& receiver() { return receiver_; }
    IBlockRegistry&  registry() { return registry_; }
    const IClock&    clock() const { return clock_; }
    /// receiver().decode(payload.bufferId)
    Result<FullDataSource> decodePayload(const net::FullDataPayload& payload);

    // listeners
    std::uint64_t addSessionConfigListener(std::function<void()> listener);
    void          removeSessionConfigListener(std::uint64_t id);
    void          setLevelInitHandler(std::function<void(const net::LevelInitMessage&)> handler);
    /// Real-time updates for `levelName` (DhClientLevel's FullDataPartialUpdate handler). The source is
    /// already decoded; the beacons come from the payload header.
    void setPartialUpdateHandler(
        const std::string&                                                               levelName,
        std::function<void(Result<FullDataSource>, const std::vector<net::BeaconBeam>&)> handler
    );

    ClientCongestionControl& congestion() { return congestion_; }
    void                     setAdaptiveTransferSpeed(bool enabled);
    bool                     adaptiveTransferSpeed() const { return adaptive_.load(); }

private:
    void noteReplyLocked(const char* reason);
    void failAllPending();
    void handleResponse(std::uint32_t wireId, NetResponse response);
    bool send(const net::Message& message);

    ITransport&     transport_;
    const IClock&   clock_;
    IBlockRegistry& registry_;
    PayloadReceiver receiver_;

    ClientCongestionControl congestion_;
    std::atomic<bool>       adaptive_{false};
    std::atomic<bool>       incompatible_{false};

    mutable std::mutex                                mutex_;
    net::SessionConfigValues                          wanted_;
    net::SessionConfigValues                          sessionConfig_;
    SessionId                                         session_ = 0;
    IExecutor*                                        work_    = nullptr;
    std::string                                       closeReason_;
    DhMode                                            mode_             = DhMode::Pending;
    std::int64_t                                      windowStartMs_    = 0;
    std::int64_t                                      modeChangedMs_    = 0;
    std::string                                       modeReason_;
    std::atomic<std::uint64_t>                        sections_{0};
    bool                                              connected_        = false;
    bool                                              configReceived_   = false;
    ServerSupport                                     support_          = ServerSupport::None;
    std::int64_t                                      serverTimeOffset_ = 0;
    std::uint32_t                                     nextCounter_      = 1;
    std::map<std::uint32_t, ResponseCallback>         pending_;
    std::map<std::uint64_t, std::function<void()>>    configListeners_;
    std::uint64_t                                     nextListener_ = 1;
    std::function<void(const net::LevelInitMessage&)> levelInitHandler_;
    std::map<std::string, std::function<void(Result<FullDataSource>, const std::vector<net::BeaconBeam>&)>>
        partialHandlers_;
};

} // namespace dh::mp
