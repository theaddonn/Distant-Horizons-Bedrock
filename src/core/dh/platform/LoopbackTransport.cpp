#include "dh/platform/ITransport.h"

#include <utility>

namespace dh {

// ---- LoopbackTransport -----------------------------------------------------

bool LoopbackTransport::send(SessionId session, std::span<const std::byte> frame) {
    if (!link_.enqueueFrame(side_, session, frame)) {
        ++rejectedSends_;
        return false;
    }
    return true;
}

std::size_t LoopbackTransport::maxFrameBytes() const { return link_.maxFrameBytes_; }

void LoopbackTransport::disconnect(SessionId session) { link_.disconnect(session); }

// ---- LoopbackLink ----------------------------------------------------------

LoopbackLink::LoopbackLink(std::size_t maxFrameBytes)
: maxFrameBytes_(maxFrameBytes),
  client_(*this, 0),
  server_(*this, 1) {}

SessionId LoopbackLink::connect(std::string clientName, std::string serverName) {
    SessionId id  = nextSession_++;
    sessions_[id] = true;
    queue_.push_back(Event{Kind::Connected, 0, id, std::move(serverName), {}});
    queue_.push_back(Event{Kind::Connected, 1, id, std::move(clientName), {}});
    return id;
}

void LoopbackLink::disconnect(SessionId session) {
    auto it = sessions_.find(session);
    if (it == sessions_.end() || !it->second) return;
    it->second = false;
    queue_.push_back(Event{Kind::Disconnected, 0, session, {}, {}});
    queue_.push_back(Event{Kind::Disconnected, 1, session, {}, {}});
}

bool LoopbackLink::isOpen(SessionId session) const {
    auto it = sessions_.find(session);
    return it != sessions_.end() && it->second;
}

bool LoopbackLink::enqueueFrame(int fromSide, SessionId session, std::span<const std::byte> frame) {
    if (frame.size() > maxFrameBytes_) return false;
    if (!isOpen(session)) return false;
    queue_.push_back(Event{Kind::Frame, 1 - fromSide, session, {}, std::vector<std::byte>(frame.begin(), frame.end())});
    return true;
}

void LoopbackLink::deliver(const Event& e) {
    LoopbackTransport&  target   = e.toSide == 0 ? client_ : server_;
    ITransportListener* listener = target.listener();
    if (!listener) return;
    switch (e.kind) {
    case Kind::Connected:
        listener->onConnected(e.session, e.peerName);
        break;
    case Kind::Frame:
        listener->onFrame(e.session, std::span<const std::byte>(e.frame));
        break;
    case Kind::Disconnected:
        listener->onDisconnected(e.session);
        break;
    }
}

std::size_t LoopbackLink::pump() {
    std::size_t n = queue_.size();
    for (std::size_t i = 0; i < n && !queue_.empty(); ++i) {
        Event e = std::move(queue_.front());
        queue_.pop_front();
        // Frames for a session that was closed before delivery are still
        // delivered if they were queued before the Disconnected event (FIFO),
        // matching a real ordered channel.
        deliver(e);
    }
    return n;
}

std::size_t LoopbackLink::pumpAll(std::size_t maxRounds) {
    std::size_t total = 0;
    for (std::size_t round = 0; round < maxRounds && !queue_.empty(); ++round) total += pump();
    return total;
}

} // namespace dh
