#pragma once

// Byte-frame transport between a DH client and a DH server (Java: the plugin
// packet channel behind IPluginPacketSender + NetworkSession; one Java
// NetworkSession per connected player).
//
// The core (WP-K1/K2, dh/net, dh/multiplayer) frames its own messages
// (version|id|payload, big-endian) and hands the platform opaque byte frames.
// The platform owns connections and identifies each remote with a SessionId.
//
// Contract:
//   * One frame in == one frame out, intact and in order per session. The
//     transport may split/reassemble internally but never exposes partial frames.
//   * send() returns false if the frame exceeds maxFrameBytes(), the session is
//     unknown/closed, or the transport is shut down. The frame is dropped.
//   * maxFrameBytes() is constant for the transport's lifetime; the core splits
//     payloads (FullDataSplit) to stay under it.
//   * Listener callbacks arrive on an unspecified platform thread, one at a time
//     per transport (never concurrently for the same transport). The core hops
//     to its own executors before doing real work.
//   * onConnected(id, peerName) precedes any onFrame(id, ...); onDisconnected(id)
//     is the last event for that id and ids are never reused.
//   * setListener(nullptr) detaches; events while detached are dropped.
//
// Implementations: glue PacketTransport (ll::network PacketRegistrar custom
// packet, MTU-sized frames), LoopbackTransport below (tests).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dh {

using SessionId = std::uint64_t;

struct ITransportListener {
    virtual ~ITransportListener()                                             = default;
    virtual void onConnected(SessionId session, std::string_view peerName)    = 0;
    virtual void onFrame(SessionId session, std::span<const std::byte> frame) = 0;
    virtual void onDisconnected(SessionId session)                            = 0;
};

struct ITransport {
    virtual ~ITransport() = default;

    virtual bool        send(SessionId session, std::span<const std::byte> frame) = 0;
    virtual std::size_t maxFrameBytes() const                                     = 0;
    virtual void        setListener(ITransportListener* listener)                 = 0;

    /// Closes `session` from this side; the peer sees onDisconnected. No-op if unknown.
    virtual void disconnect(SessionId session) = 0;
};

class LoopbackLink;

/// One end of a LoopbackLink. Only constructible by LoopbackLink.
class LoopbackTransport final : public ITransport {
public:
    bool        send(SessionId session, std::span<const std::byte> frame) override;
    std::size_t maxFrameBytes() const override;
    void        setListener(ITransportListener* listener) override { listener_ = listener; }
    void        disconnect(SessionId session) override;

    ITransportListener* listener() const { return listener_; }

    /// Frames this endpoint tried to send that were rejected (too large / closed).
    std::size_t rejectedSends() const { return rejectedSends_; }

private:
    friend class LoopbackLink;
    LoopbackTransport(LoopbackLink& link, int side) : link_(link), side_(side) {}

    LoopbackLink&       link_;
    int                 side_;
    ITransportListener* listener_      = nullptr;
    std::size_t         rejectedSends_ = 0;
};

/// In-process pair of transports for tests. Every event (connect, frame,
/// disconnect) is queued and delivered only on pump(), FIFO across both
/// directions, so tests control interleaving exactly.
///
///   LoopbackLink link(1024);
///   link.client().setListener(&c); link.server().setListener(&s);
///   SessionId id = link.connect();     // queued: onConnected on both sides
///   link.client().send(id, bytes);     // queued
///   link.pump();                       // delivers everything queued so far
class LoopbackLink {
public:
    explicit LoopbackLink(std::size_t maxFrameBytes = 32 * 1024);

    LoopbackTransport& client() { return client_; }
    LoopbackTransport& server() { return server_; }

    /// Creates a session visible on both ends under the same id. Queues
    /// onConnected(id, "server") for the client and onConnected(id, "client")
    /// for the server.
    SessionId connect(std::string clientName = "client", std::string serverName = "server");

    /// Queues onDisconnected(id) on both sides and marks the session closed
    /// (further send()s return false immediately).
    void disconnect(SessionId session);

    /// Delivers the events that were queued when the call started (one "tick").
    /// Events queued by listeners during delivery wait for the next pump().
    /// Returns the number of events delivered.
    std::size_t pump();

    /// Pumps until nothing is queued (bounded by maxRounds). Returns events delivered.
    std::size_t pumpAll(std::size_t maxRounds = 10'000);

    std::size_t pending() const { return queue_.size(); }
    bool        isOpen(SessionId session) const;

private:
    friend class LoopbackTransport;

    enum class Kind { Connected, Frame, Disconnected };
    struct Event {
        Kind                   kind;
        int                    toSide; ///< 0 = client, 1 = server
        SessionId              session;
        std::string            peerName;
        std::vector<std::byte> frame;
    };

    bool enqueueFrame(int fromSide, SessionId session, std::span<const std::byte> frame);
    void deliver(const Event& e);

    std::size_t               maxFrameBytes_;
    LoopbackTransport         client_;
    LoopbackTransport         server_;
    std::deque<Event>         queue_;
    std::map<SessionId, bool> sessions_; ///< id -> open
    SessionId                 nextSession_ = 1;
};

} // namespace dh
