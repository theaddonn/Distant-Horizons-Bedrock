#pragma once

// Server networking glue: PacketTransport -> dh::mp::ServerNetwork (+ ServerPlayers, ServerSettings).
//
//   * join / disconnect events open / close a transport session (player = session), which drives
//     ServerNetwork::onConnected / onDisconnected on the transport's dispatch thread.
//   * ServerSettings are derived from the shared options (Config.Server.* and Config.Common.WorldGenerator.*);
//     a config change only raises a flag, the server tick applies it and calls broadcastConfig()
//     (ServerPlayers::settings must be mutated on the tick thread only).
//   * per-dimension request handlers are created by ServerWorldDriver and registered here.
//
// Threading: every method belongs to the server thread; the join / leave callbacks run on it too, and config
// changes (any thread) only raise a flag.

#include "bedrock_common/PacketTransport.h"

#include "dh/config/ConfigStore.h"
#include "dh/multiplayer/ServerRequestHandler.h"
#include "dh/platform/IClock.h"

#include "ll/api/event/ListenerBase.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace dh::bedrock {

class NetServer {
public:
    NetServer(PacketTransport& transport, dh::IClock& clock, dh::ConfigStore& config);
    ~NetServer();
    NetServer(const NetServer&)            = delete;
    NetServer& operator=(const NetServer&) = delete;

    /// Attaches the network to the transport and subscribes to player join / disconnect and config changes.
    void start();
    /// Unsubscribes and detaches (waits for a running transport callback). Idempotent.
    void stop();

    dh::mp::ServerPlayers& players() { return players_; }
    dh::mp::ServerNetwork& network() { return network_; }

    /// Server thread, every server tick (20 Hz): applies a pending config change (settings + broadcastConfig).
    void applyPendingConfig();

    /// Server settings derived from the options (Java ServerPlayerStateManager reading Config.Server.*).
    static dh::mp::ServerSettings settingsFrom(const dh::Options& options);

    /// FullDataSourceRequest.levelName / RequestLevelInit facts for one dimension.
    static dh::mp::LevelInfo levelInfoFor(
        const std::string& worldName,
        const std::string& dimensionResource,
        double             coordinateScale,
        const dh::Options& options
    );

    /// Server thread: the player's level (dimension) changed; re-sends his SessionConfig (generation centre depends
    /// on the dimension coordinate scale, Java ServerPlayerState.sendConfigMessage).
    void onPlayerLevelChanged(dh::SessionId session);

    /// Runs on the server thread when a player joined (after the session opened).
    void setJoinHook(std::function<void()> hook) { joinHook_ = std::move(hook); }
    /// False while the (live) generator plan is Disabled; readable from any thread.
    const std::atomic<bool>& generationAllowed() const { return generationAllowed_; }

    std::size_t playerCount() const { return transport_.sessionCount(); }

private:
    void logEffectiveSettings() const;

    PacketTransport&      transport_;
    dh::ConfigStore&      config_;
    dh::mp::ServerPlayers players_;
    dh::mp::ServerNetwork network_;

    std::shared_ptr<std::atomic<bool>> configDirty_ = std::make_shared<std::atomic<bool>>(false);
    std::function<void()>              joinHook_;
    std::atomic<bool>                  generationAllowed_{true};
    bool                               started_ = false;
    ll::event::ListenerPtr             joinListener_;
    ll::event::ListenerPtr             leaveListener_;
};

} // namespace dh::bedrock
