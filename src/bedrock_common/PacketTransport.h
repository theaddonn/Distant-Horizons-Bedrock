#pragma once

// dh::ITransport over a LeviLamina custom network packet ("dh:frame"), shared by the client and the
// server DLL (see docs/spikes/server-driver.md).
//
// Carrier: ll::network::PacketBase<DhFramePacket> registered through ll::network::PacketRegistrar. Both
// the client and the server LeviLamina.dll export PacketRegistrar / RuntimePacket (verified: the symbols
// are in both bin/LeviLamina.dll), and ll::network::Packet::sendToServer() (client) / sendToClient()
// (server) exist in both header sets, so ScriptMessagePacket is NOT needed. The packet carries one DH
// frame (the K1 wire frame: version|id|payload) as a length-prefixed byte string.
//
// Frame size: maxFrameBytes() = 32 KiB. RakNet fragments a reliable message above the ~1.4 KB MTU and
// reassembles it, and Bedrock batches/compresses, so the real ceiling is the batch limit (about 1 MiB
// compressed); 32 KiB keeps one frame well inside it, leaves room for other traffic between frames and
// keeps head-of-line blocking short. The core splits payloads (FullDataSplit) to this size.
//
// Threading (contract: events arrive one at a time and in order, never inline):
//   * Inbound: the packet handler (game/network thread) only enqueues; a private single-thread executor
//     delivers onConnected / onFrame / onDisconnected to the listener in order, so the listener never runs
//     on the game thread and the core can block briefly.
//   * Outbound: send() may be called from any core worker thread; it only queues. The game thread calls
//     flushOutbound() once per tick, which does the actual packet sends (Bedrock's packet sender is not
//     documented as thread safe, so no packet is ever sent off the game thread).
//
// Server role (LL_PLAT_S): SessionId per player. openSession()/closeSession() are driven by the join /
// disconnect events (NetServer). Frames from an unknown NetworkIdentifier are dropped.
// Client role: a single session to the server, opened/closed with clientConnect()/clientDisconnect().
// SessionIds are never reused.

#include "dh/platform/ITransport.h"
#include "dh/thread/SingleThreadExecutor.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef LL_PLAT_S
#include "mc/common/SubClientId.h"
#include "mc/network/NetworkIdentifier.h"
#endif

namespace dh::bedrock {

class PacketTransport final : public dh::ITransport {
public:
    /// Frames above this are rejected by send() (see header comment).
    static constexpr std::size_t kMaxFrameBytes = 32 * 1024;
    /// Receive-side sanity cap for a packet's byte string (kMaxFrameBytes plus slack).
    static constexpr std::size_t kMaxPacketBytes = kMaxFrameBytes + 1024;

    PacketTransport();
    ~PacketTransport() override;
    PacketTransport(const PacketTransport&)            = delete;
    PacketTransport& operator=(const PacketTransport&) = delete;

    /// The live instance the packet handler routes to (nullptr before construction / after destruction).
    static PacketTransport* active();

    // ITransport
    bool        send(dh::SessionId session, std::span<const std::byte> frame) override;
    std::size_t maxFrameBytes() const override { return kMaxFrameBytes; }
    void        setListener(dh::ITransportListener* listener) override;
    /// Closes the DH session only (the Bedrock connection stays up, the peer is not told: the core sends
    /// CloseReason first when it matters). Local onDisconnected follows; later frames are dropped.
    void disconnect(dh::SessionId session) override;

    /// Game thread, once per tick: sends the queued outbound frames. Returns the number sent.
    std::size_t flushOutbound();

    /// Detaches the listener, drops queued frames, stops the dispatch thread. Idempotent.
    void shutdown();

    std::uint64_t framesSent() const { return framesSent_.load(); }
    std::uint64_t framesReceived() const { return framesReceived_.load(); }
    std::uint64_t framesDropped() const { return framesDropped_.load(); }
    std::size_t   sessionCount() const;

#ifdef LL_PLAT_S
    /// Join: allocates a session for the player's connection and reports onConnected(id, peerName).
    dh::SessionId openSession(const NetworkIdentifier& id, SubClientId subId, std::string peerName);
    /// Disconnect: reports onDisconnected for the session of `id` (no-op if unknown).
    void closeSession(const NetworkIdentifier& id);
    /// 0 if the connection has no session.
    dh::SessionId sessionOf(const NetworkIdentifier& id) const;
    /// Called by the packet handler for every received dh:frame (game thread).
    void receive(const NetworkIdentifier& from, std::string&& payload);
#else
    /// The client entered a server world: opens the single session (returns its id).
    dh::SessionId clientConnect(std::string peerName = "server");
    /// Left the world / disconnected: reports onDisconnected.
    void clientDisconnect();
    /// Called by the packet handler for every received dh:frame (game thread).
    void receive(std::string&& payload);
#endif

private:
    struct Outbound {
        dh::SessionId session = 0;
        std::string   bytes;
    };
    void post(std::function<void()> task);

    mutable std::mutex      mutex_;         ///< sessions, outbound queue
    std::mutex              listenerMutex_; ///< held while a callback runs, so setListener(nullptr) waits for it
    dh::ITransportListener* listener_ = nullptr;
    std::deque<Outbound>    outbound_;
    std::atomic<bool>       closed_{false};
    dh::SessionId           nextSession_ = 1;

#ifdef LL_PLAT_S
    struct Peer {
        NetworkIdentifier id;
        SubClientId       subId;
    };
    std::unordered_map<NetworkIdentifier, dh::SessionId> byId_;
    std::unordered_map<dh::SessionId, Peer>              bySession_;
    std::unordered_set<dh::SessionId>                    closing_; ///< disconnect() called, flushed next tick
#else
    dh::SessionId clientSession_ = 0;
#endif

    std::atomic<std::uint64_t>                framesSent_{0}, framesReceived_{0}, framesDropped_{0};
    std::unique_ptr<dh::SingleThreadExecutor> dispatch_; ///< declared last: joined first on destruction
};

} // namespace dh::bedrock
