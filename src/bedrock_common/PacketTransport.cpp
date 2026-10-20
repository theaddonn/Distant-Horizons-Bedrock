#include "bedrock_common/PacketTransport.h"

#include "bedrock_common/FileLogger.h"

#include "mc/deps/core/utility/optional_ref.h"

#include "ll/api/network/packet/Packet.h"
#include "mc/common/SubClientId.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/NetworkIdentifier.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <system_error>

namespace dh::bedrock {

namespace {

#ifdef LL_PLAT_S
constexpr const char* kSideName = "server";
#else
constexpr const char* kSideName = "client";
#endif

std::atomic<PacketTransport*> gActive{nullptr};

} // namespace

// Named (not anonymous) namespace: the packet id is hash(type name), so the text must be stable and identical in
// the client and the server DLL.
namespace wire {

// The packet type. LeviLamina registers it in PacketRegistrar under hash(type name) at static-init time
// (PacketBase::sRegistered), identically in the client and the server DLL (same source -> same name).
// The wire payload is one length-prefixed byte string (varint length + raw bytes; writeString is
// binary-safe). Incompressible: DH frames are already compact/zstd-able at the codec layer.
class DhFramePacket final : public ll::network::PacketBase<DhFramePacket> {
public:
    std::string payload;

    DhFramePacket()
    : PacketBase<DhFramePacket>(
          PacketPriority::MediumPriority,
          NetworkPeer::Reliability::ReliableOrdered,
          SubClientId::PrimaryClient,
          false
      ) {}

    void write(BinaryStream& stream) const override { stream.writeString(payload, nullptr, nullptr); }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto r = stream.getString(PacketTransport::kMaxPacketBytes);
        if (!r) {
            return nonstd::make_unexpected(
                Bedrock::ErrorInfo<std::error_code>{std::make_error_code(std::errc::bad_message)}
            );
        }
        payload = std::move(*r);
        return {};
    }
};

// Receives dh:frame on the game thread (server: ServerNetworkHandler dispatch, client: client dispatch).
class DhFrameHandler final : public ll::network::PacketHandlerBase<DhFrameHandler, DhFramePacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, DhFramePacket const& packet) const {
        try {
            PacketTransport* t = PacketTransport::active();
            if (!t) return;
            std::string copy = packet.payload;
#ifdef LL_PLAT_S
            t->receive(netId, std::move(copy));
#else
            (void)netId;
            t->receive(std::move(copy));
#endif
        } catch (...) {
            DH_LOG_E("transport: exception swallowed in dh:frame handler");
        }
    }
};

} // namespace wire

using wire::DhFramePacket;

PacketTransport::PacketTransport() : dispatch_(std::make_unique<dh::SingleThreadExecutor>("DH Net Dispatch")) {
    gActive.store(this);
    try {
        // P1: the id must be identical in both DLLs (hash of the class name); logged per side.
        DhFramePacket probe;
        DH_LOG_I(
            "transport: %s packet '%.*s' runtime id %llu (0x%llx), max frame %zu bytes",
            kSideName,
            (int)probe.getName().size(),
            probe.getName().data(),
            (unsigned long long)probe.getRuntimeId(),
            (unsigned long long)probe.getRuntimeId(),
            kMaxFrameBytes
        );
    } catch (...) {
        DH_LOG_E("transport: exception swallowed while logging the packet id");
    }
}

PacketTransport::~PacketTransport() {
    shutdown();
    PacketTransport* self = this;
    gActive.compare_exchange_strong(self, nullptr);
}

PacketTransport* PacketTransport::active() { return gActive.load(); }

void PacketTransport::setListener(dh::ITransportListener* listener) {
    std::lock_guard lock(listenerMutex_); // waits for a running callback
    listener_ = listener;
}

void PacketTransport::post(std::function<void()> task) {
    if (closed_.load() || !dispatch_) return;
    (void)dispatch_->execute([task = std::move(task)] {
        try {
            task();
        } catch (...) {
            DH_LOG_E("transport: exception swallowed in listener callback");
        }
    });
}

bool PacketTransport::send(dh::SessionId session, std::span<const std::byte> frame) {
    if (closed_.load() || frame.size() > kMaxFrameBytes || frame.empty()) {
        ++framesDropped_;
        return false;
    }
    std::lock_guard lock(mutex_);
#ifdef LL_PLAT_S
    if (!bySession_.contains(session) || closing_.contains(session)) {
        ++framesDropped_;
        return false;
    }
#else
    if (session == 0 || session != clientSession_) {
        ++framesDropped_;
        return false;
    }
#endif
    Outbound o;
    o.session = session;
    o.bytes.assign(reinterpret_cast<const char*>(frame.data()), frame.size());
    outbound_.push_back(std::move(o));
    return true;
}

std::size_t PacketTransport::flushOutbound() {
    std::deque<Outbound> batch;
#ifdef LL_PLAT_S
    std::unordered_set<dh::SessionId> closingNow; // taken with the swap: their last frames are in `batch`
#endif
    {
        std::lock_guard lock(mutex_);
        batch.swap(outbound_);
#ifdef LL_PLAT_S
        closingNow.swap(closing_);
#endif
    }
    std::size_t sent = 0;
    for (auto& o : batch) {
        try {
            DhFramePacket packet;
#ifdef LL_PLAT_S
            std::optional<Peer> peer; // NetworkIdentifier has no exported default constructor
            {
                std::lock_guard lock(mutex_);
                auto            it = bySession_.find(o.session);
                if (it != bySession_.end()) peer.emplace(it->second);
            }
            if (!peer) {
                ++framesDropped_;
                continue;
            }
            packet.payload = std::move(o.bytes);
            packet.sendToClient(peer->id, peer->subId);
#else
            packet.payload = std::move(o.bytes);
            packet.sendToServer();
#endif
            ++sent;
            if (framesSent_.fetch_add(1) == 0)
                DH_LOG_I("transport: %s sent its first dh:frame (%zu bytes)", kSideName, packet.payload.size());
        } catch (...) {
            ++framesDropped_;
            DH_LOG_E("transport: exception swallowed while sending a dh:frame");
        }
    }
#ifdef LL_PLAT_S
    {
        // Sessions closed by disconnect(): their last frames (CloseReason) went out above; now forget them.
        std::lock_guard lock(mutex_);
        for (dh::SessionId s : closingNow) {
            auto it = bySession_.find(s);
            if (it == bySession_.end()) continue;
            byId_.erase(it->second.id);
            bySession_.erase(it);
        }
    }
#endif
    return sent;
}

void PacketTransport::shutdown() {
    if (closed_.exchange(true)) return;
    setListener(nullptr);
    {
        std::lock_guard lock(mutex_);
        outbound_.clear();
    }
    if (dispatch_) {
        dispatch_->shutdownNow();
        dispatch_->join();
    }
}

#ifdef LL_PLAT_S

std::size_t PacketTransport::sessionCount() const {
    std::lock_guard lock(mutex_);
    return bySession_.size();
}

dh::SessionId PacketTransport::openSession(const NetworkIdentifier& id, SubClientId subId, std::string peerName) {
    dh::SessionId session;
    {
        std::lock_guard lock(mutex_);
        if (auto it = byId_.find(id); it != byId_.end()) return it->second;
        session = nextSession_++;
        byId_.emplace(id, session);
        bySession_.emplace(session, Peer{id, subId});
    }
    post([this, session, name = std::move(peerName)] {
        std::lock_guard lock(listenerMutex_);
        if (listener_) listener_->onConnected(session, name);
    });
    return session;
}

void PacketTransport::closeSession(const NetworkIdentifier& id) {
    dh::SessionId session = 0;
    {
        std::lock_guard lock(mutex_);
        auto            it = byId_.find(id);
        if (it == byId_.end()) return;
        session = it->second;
        byId_.erase(it);
        bySession_.erase(session);
        std::erase_if(outbound_, [&](const Outbound& o) { return o.session == session; });
        // disconnect() already reported onDisconnected for this session: do not report it twice.
        if (closing_.erase(session) != 0) return;
    }
    post([this, session] {
        std::lock_guard lock(listenerMutex_);
        if (listener_) listener_->onDisconnected(session);
    });
}

dh::SessionId PacketTransport::sessionOf(const NetworkIdentifier& id) const {
    std::lock_guard lock(mutex_);
    auto            it = byId_.find(id);
    return it == byId_.end() ? 0 : it->second;
}

void PacketTransport::disconnect(dh::SessionId session) {
    {
        std::lock_guard lock(mutex_);
        if (!bySession_.contains(session) || !closing_.insert(session).second) return;
        // The session stays mapped until the next flushOutbound() so frames queued just before (the
        // core's CloseReason) still go out; send()/receive() already treat it as closed.
    }
    post([this, session] {
        std::lock_guard lock(listenerMutex_);
        if (listener_) listener_->onDisconnected(session);
    });
}

void PacketTransport::receive(const NetworkIdentifier& from, std::string&& payload) {
    dh::SessionId session = sessionOf(from);
    {
        std::lock_guard lock(mutex_);
        if (closing_.contains(session)) session = 0;
    }
    if (session == 0 || payload.empty() || payload.size() > kMaxPacketBytes) {
        ++framesDropped_;
        return;
    }
    if (framesReceived_.fetch_add(1) == 0)
        DH_LOG_I("transport: %s received its first dh:frame (%zu bytes)", kSideName, payload.size());
    post([this, session, bytes = std::move(payload)] {
        std::lock_guard lock(listenerMutex_);
        if (listener_)
            listener_->onFrame(
                session,
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size())
            );
    });
}

#else // client

std::size_t PacketTransport::sessionCount() const {
    std::lock_guard lock(mutex_);
    return clientSession_ != 0 ? 1 : 0;
}

dh::SessionId PacketTransport::clientConnect(std::string peerName) {
    dh::SessionId session;
    {
        std::lock_guard lock(mutex_);
        if (clientSession_ != 0) return clientSession_;
        session        = nextSession_++;
        clientSession_ = session;
    }
    post([this, session, name = std::move(peerName)] {
        std::lock_guard lock(listenerMutex_);
        if (listener_) listener_->onConnected(session, name);
    });
    return session;
}

void PacketTransport::clientDisconnect() {
    dh::SessionId session;
    {
        std::lock_guard lock(mutex_);
        session        = clientSession_;
        clientSession_ = 0;
        outbound_.clear();
    }
    if (session == 0) return;
    post([this, session] {
        std::lock_guard lock(listenerMutex_);
        if (listener_) listener_->onDisconnected(session);
    });
}

void PacketTransport::disconnect(dh::SessionId session) {
    {
        std::lock_guard lock(mutex_);
        if (session == 0 || session != clientSession_) return;
    }
    clientDisconnect();
}

void PacketTransport::receive(std::string&& payload) {
    dh::SessionId session;
    {
        std::lock_guard lock(mutex_);
        session = clientSession_;
    }
    if (session == 0 || payload.empty() || payload.size() > kMaxPacketBytes) {
        ++framesDropped_;
        return;
    }
    if (framesReceived_.fetch_add(1) == 0)
        DH_LOG_I("transport: %s received its first dh:frame (%zu bytes)", kSideName, payload.size());
    post([this, session, bytes = std::move(payload)] {
        std::lock_guard lock(listenerMutex_);
        if (listener_)
            listener_->onFrame(
                session,
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size())
            );
    });
}

#endif

} // namespace dh::bedrock
