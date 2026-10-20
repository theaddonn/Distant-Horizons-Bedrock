#include "dh/multiplayer/ClientNetworkState.h"

#include "dh/net/MessageRegistry.h"
#include "dh/platform/ILogger.h"

#include <algorithm>

namespace dh::mp {

//------------------------------------------------------------------------------
// ClientCongestionControl
//------------------------------------------------------------------------------

ClientCongestionControl::ClientCongestionControl(const IClock& clock, std::function<void()> rateUpdateHandler)
: clock_(clock),
  rateUpdateHandler_(std::move(rateUpdateHandler)) {
    reset();
}

void ClientCongestionControl::reset() {
    std::lock_guard lock(mutex_);
    desiredRate_   = kAdditiveIncrease;
    lastAdjustMs_  = clock_.nowMillis();
    bytesReceived_ = 0;
}

void ClientCongestionControl::onPayloadReceived(std::size_t chunkBytes) {
    bool adjusted = false;
    {
        std::lock_guard    lock(mutex_);
        const std::int64_t now = clock_.nowMillis();
        if (now - lastAdjustMs_ >= kIntervalMs) {
            const double throughput = static_cast<double>(bytesReceived_);
            bytesReceived_          = 0;
            if (throughput >= desiredRate_) {
                desiredRate_ += kAdditiveIncrease;
            } else {
                desiredRate_ = std::max(throughput - kAdditiveIncrease / 2, 1000.0);
            }
            lastAdjustMs_ = now;
            adjusted      = true;
        }
        bytesReceived_ += chunkBytes;
    }
    if (adjusted && rateUpdateHandler_) rateUpdateHandler_(); // outside the lock: it reads getDesiredRate()
}

int ClientCongestionControl::getDesiredRate() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(desiredRate_ / 1000);
}

//------------------------------------------------------------------------------
// ClientNetworkState
//------------------------------------------------------------------------------

ClientNetworkState::ClientNetworkState(
    ITransport&     transport,
    const IClock&   clock,
    IBlockRegistry& registry,
    ClientNetConfig config
)
: transport_(transport),
  clock_(clock),
  registry_(registry),
  receiver_(clock),
  congestion_(
      clock,
      [this] {
          if (adaptive_.load()) sendConfigMessage(false);
      }
  ),
  wanted_(config.wanted) {
    adaptive_.store(config.adaptiveTransferSpeed);
    windowStartMs_ = modeChangedMs_ = clock_.nowMillis();
    transport_.setListener(this);
}

ClientNetworkState::~ClientNetworkState() { transport_.setListener(nullptr); }

bool ClientNetworkState::send(const net::Message& message) {
    SessionId session;
    {
        std::lock_guard lock(mutex_);
        if (!connected_) return false;
        session = session_;
    }
    return transport_.send(session, net::encodeFrame(message));
}

bool ClientNetworkState::isConnected() const {
    std::lock_guard lock(mutex_);
    return connected_;
}
bool ClientNetworkState::isReady() const {
    std::lock_guard lock(mutex_);
    return configReceived_;
}
net::SessionConfigValues ClientNetworkState::sessionConfig() const {
    std::lock_guard lock(mutex_);
    return sessionConfig_;
}
std::int64_t ClientNetworkState::serverTimeOffset() const {
    std::lock_guard lock(mutex_);
    return serverTimeOffset_;
}
ServerSupport ClientNetworkState::serverSupport() const {
    std::lock_guard lock(mutex_);
    return support_;
}
std::size_t ClientNetworkState::pendingRequestCount() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

std::string ClientNetworkState::debugStatus() const {
    std::lock_guard lock(mutex_);
    if (!connected_ || !closeReason_.empty()) return "NetworkSession closed: " + closeReason_;
    if (support_ == ServerSupport::None && incompatible_.load()) return "Incompatible protocol version";
    switch (support_) {
    case ServerSupport::None:
        return "Server does not support DH";
    case ServerSupport::LevelsOnly:
        return "Server supports shared level keys";
    case ServerSupport::Full:
        return "Server has full DH support";
    }
    return {};
}

void ClientNetworkState::setAdaptiveTransferSpeed(bool enabled) {
    adaptive_.store(enabled);
    if (enabled) congestion_.reset(); // ConfigChangeListener
    sendConfigMessage(true);
}

void ClientNetworkState::sendConfigMessage(bool blocking) {
    net::SessionConfigMessage m;
    {
        std::lock_guard lock(mutex_);
        m.config = wanted_;
    }
    if (adaptive_.load()) {
        // sessionConfig.constrainValue(playerBandwidthLimit, desiredRate): configured (if > 0) and desired, whichever
        // is lower
        const int desired             = congestion_.getDesiredRate();
        const int cfg                 = m.config.playerBandwidthLimit;
        m.config.playerBandwidthLimit = cfg <= 0 ? desired : std::min(cfg, desired);
    }
    if (blocking) {
        std::lock_guard lock(mutex_);
        configReceived_ = false;
    }
    send(m);
}

void ClientNetworkState::setWanted(const net::SessionConfigValues& values) {
    {
        std::lock_guard lock(mutex_);
        wanted_ = values;
    }
    sendConfigMessage(true); // Java configAnyChangeListener
}

void ClientNetworkState::setWorkExecutor(IExecutor* executor) {
    std::lock_guard lock(mutex_);
    work_ = executor;
}

void ClientNetworkState::sendLevelInitRequest(const std::string& dimensionResourceLocation) {
    net::RequestLevelInitMessage m;
    m.dimensionResourceLocation = dimensionResourceLocation;
    send(m);
}

std::uint64_t ClientNetworkState::sendRequest(
    const std::string&          levelName,
    std::uint64_t               sectionPos,
    std::optional<std::int64_t> clientTimestamp,
    ResponseCallback            callback
) {
    net::FullDataSourceRequestMessage m;
    m.levelName       = levelName;
    m.sectionPos      = static_cast<std::int64_t>(sectionPos);
    m.clientTimestamp = clientTimestamp;

    std::uint32_t wire;
    {
        std::lock_guard lock(mutex_);
        if (!connected_) {
            wire = 0;
        } else {
            do {
                wire =
                    net::FutureId::wire(net::FutureId::make(static_cast<std::uint32_t>(session_), false, nextCounter_));
                nextCounter_ = (nextCounter_ + 1) & 0x7FFFFFFFu;
                if (nextCounter_ == 0) nextCounter_ = 1;
            } while (pending_.count(wire) != 0);
            pending_[wire] = callback;
        }
    }
    if (wire == 0) {
        NetResponse r;
        r.kind    = NetResponse::Kind::Closed;
        r.message = "not connected";
        if (callback) callback(r);
        return 0;
    }
    m.futureId = wire;
    if (!send(m)) {
        {
            std::lock_guard lock(mutex_);
            pending_.erase(wire);
        }
        NetResponse r;
        r.kind    = NetResponse::Kind::Closed;
        r.message = "send failed";
        if (callback) callback(r);
        return 0;
    }
    return wire;
}

void ClientNetworkState::cancelRequest(std::uint64_t futureId) {
    bool existed;
    {
        std::lock_guard lock(mutex_);
        existed = pending_.erase(net::FutureId::wire(futureId)) != 0;
    }
    if (!existed) return;
    net::CancelMessage m;
    m.futureId = futureId;
    send(m);
}

Result<FullDataSource> ClientNetworkState::decodePayload(const net::FullDataPayload& payload) {
    return receiver_.decode(payload.dtoBufferId, registry_);
}

Result<FullDataSource> ClientNetworkState::decodePayload(const net::FullDataPayload& payload, std::size_t* bytes) {
    auto raw = receiver_.take(payload.dtoBufferId);
    if (!raw) return Error(ErrorCode::NotFound, "no complete buffer for a received payload");
    if (bytes) *bytes = raw->size();
    return decodePayloadBytes(*raw, registry_);
}

std::uint64_t ClientNetworkState::addSessionConfigListener(std::function<void()> listener) {
    std::lock_guard lock(mutex_);
    const auto      id   = nextListener_++;
    configListeners_[id] = std::move(listener);
    return id;
}
void ClientNetworkState::removeSessionConfigListener(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    configListeners_.erase(id);
}
void ClientNetworkState::setLevelInitHandler(std::function<void(const net::LevelInitMessage&)> handler) {
    std::lock_guard lock(mutex_);
    levelInitHandler_ = std::move(handler);
}
void ClientNetworkState::setPartialUpdateHandler(
    const std::string&                                                               levelName,
    std::function<void(Result<FullDataSource>, const std::vector<net::BeaconBeam>&)> handler
) {
    std::lock_guard lock(mutex_);
    if (handler) partialHandlers_[levelName] = std::move(handler);
    else partialHandlers_.erase(levelName);
}

DhMode ClientNetworkState::dhMode() {
    std::lock_guard lock(mutex_);
    if (mode_ == DhMode::Pending && clock_.nowMillis() - windowStartMs_ >= kServerReplyTimeoutMs) {
        mode_          = DhMode::ClientOnly;
        modeChangedMs_ = clock_.nowMillis();
        modeReason_    = "no DH reply";
    }
    return mode_;
}

std::int64_t ClientNetworkState::modeDecisionMs() const {
    std::lock_guard lock(mutex_);
    return modeChangedMs_ - windowStartMs_;
}

std::string ClientNetworkState::modeReason() const {
    std::lock_guard lock(mutex_);
    return modeReason_;
}

void ClientNetworkState::noteReplyLocked(const char* reason) {
    if (mode_ == DhMode::Server) return;
    mode_          = DhMode::Server;
    modeChangedMs_ = clock_.nowMillis();
    modeReason_    = reason;
}

void ClientNetworkState::noteSectionReceived(std::uint64_t pos, std::size_t bytes) {
    const std::uint64_t n = ++sections_;
    if (n <= 20 || n % 100 == 0)
        log::logf(
            LogLevel::Info,
            "net: rx FullDataSourceResponse pos=%llu bytes=%zu (#%llu)",
            static_cast<unsigned long long>(pos),
            bytes,
            static_cast<unsigned long long>(n)
        );
}

void ClientNetworkState::onConnected(SessionId session, std::string_view) {
    {
        std::lock_guard lock(mutex_);
        session_        = session;
        connected_      = true;
        configReceived_ = false;
        mode_           = DhMode::Pending;
        windowStartMs_  = modeChangedMs_ = clock_.nowMillis();
        modeReason_.clear();
    }
    sendConfigMessage(true);
}

void ClientNetworkState::failAllPending() {
    std::map<std::uint32_t, ResponseCallback> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, cb] : pending) {
        NetResponse r;
        r.kind    = NetResponse::Kind::Closed;
        r.message = "session closed";
        if (cb) cb(r);
    }
}

void ClientNetworkState::onDisconnected(SessionId) {
    {
        std::lock_guard lock(mutex_);
        connected_      = false;
        configReceived_ = false;
        mode_           = DhMode::Pending;
        windowStartMs_  = modeChangedMs_ = clock_.nowMillis();
        modeReason_.clear();
    }
    failAllPending();
}

void ClientNetworkState::handleResponse(std::uint32_t wireId, NetResponse response) {
    ResponseCallback cb;
    {
        std::lock_guard lock(mutex_);
        auto            it = pending_.find(wireId);
        if (it != pending_.end()) {
            cb = std::move(it->second);
            pending_.erase(it);
        }
    }
    if (!cb) {
        // cancelled or unknown: free the data that came with it
        if (response.kind == NetResponse::Kind::Payload) receiver_.discard(response.payload.dtoBufferId);
        return;
    }
    cb(response);
}

void ClientNetworkState::onFrame(SessionId, std::span<const std::byte> frame) {
    auto decoded = net::decodeFrame(frame);
    if (!decoded) {
        if (decoded.error().code == ErrorCode::Unsupported) incompatible_.store(true);
        return;
    }
    net::Message& message = **decoded;
    switch (message.id()) {
    case net::MsgIds::LevelInit: {
        const auto&                                       m = static_cast<net::LevelInitMessage&>(message);
        std::function<void(const net::LevelInitMessage&)> handler;
        {
            std::lock_guard lock(mutex_);
            if (support_ == ServerSupport::None)
                support_ = ServerSupport::LevelsOnly; // may also arrive with full support
            serverTimeOffset_ = m.serverTime - clock_.nowMillis();
            handler           = levelInitHandler_;
            noteReplyLocked("level init received");
        }
        if (handler) handler(m);
        break;
    }
    case net::MsgIds::SessionConfig: {
        const auto&                        m = static_cast<net::SessionConfigMessage&>(message);
        std::vector<std::function<void()>> listeners;
        {
            std::lock_guard lock(mutex_);
            const bool      planChanged = sessionConfig_.generatorPlan != m.config.generatorPlan;
            support_                    = ServerSupport::Full;
            sessionConfig_              = m.config;
            configReceived_             = true;
            noteReplyLocked("session config received");
            if (planChanged)
                for (auto& [id, l] : configListeners_) listeners.push_back(l);
        }
        for (auto& l : listeners) l();
        break;
    }
    case net::MsgIds::FullDataSplit: {
        const auto& m = static_cast<net::FullDataSplitMessage&>(message);
        receiver_.receiveChunk(m);
        congestion_.onPayloadReceived(m.buffer.size());
        break;
    }
    case net::MsgIds::FullDataSourceResponse: {
        auto&       m = static_cast<net::FullDataSourceResponseMessage&>(message);
        NetResponse r;
        if (m.payload) {
            r.kind    = NetResponse::Kind::Payload;
            r.payload = *m.payload;
        } else {
            r.kind = NetResponse::Kind::Empty;
        }
        handleResponse(net::FutureId::wire(m.futureId), std::move(r));
        break;
    }
    case net::MsgIds::Exception: {
        auto&       m = static_cast<net::ExceptionMessage&>(message);
        NetResponse r;
        r.kind      = NetResponse::Kind::Remote;
        r.exception = m.kind;
        r.message   = m.message;
        handleResponse(net::FutureId::wire(m.futureId), std::move(r));
        break;
    }
    case net::MsgIds::FullDataPartialUpdate: {
        auto& m = static_cast<net::FullDataPartialUpdateMessage&>(message);
        std::function<void(Result<FullDataSource>, const std::vector<net::BeaconBeam>&)> handler;
        {
            std::lock_guard lock(mutex_);
            auto            it = partialHandlers_.find(m.levelName);
            if (it != partialHandlers_.end()) handler = it->second;
        }
        if (!handler) {
            receiver_.discard(m.payload.dtoBufferId); // not our level (or nobody listens): free the buffer
            break;
        }
        IExecutor* work;
        {
            std::lock_guard lock(mutex_);
            work = work_;
        }
        // decoding is heavy: leave the transport thread (ITransport.h) when an executor is configured
        auto run = [this, handler, payload = m.payload] { handler(decodePayload(payload), payload.beaconBeams); };
        if (work == nullptr || !work->execute(run)) {
            if (work == nullptr) run();
            else receiver_.discard(m.payload.dtoBufferId);
        }
        break;
    }
    case net::MsgIds::CloseReason: {
        {
            std::lock_guard lock(mutex_);
            configReceived_ = false;
            closeReason_    = static_cast<net::CloseReasonMessage&>(message).reason;
        }
        failAllPending();
        break;
    }
    default:
        break;
    }
}

} // namespace dh::mp
