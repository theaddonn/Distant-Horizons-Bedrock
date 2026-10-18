#include "dh/multiplayer/ServerRequestHandler.h"

#include "dh/multiplayer/WorldGenRange.h"
#include "dh/net/MessageRegistry.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/util/Bits.h"

#include <algorithm>
#include <cctype>

namespace dh::mp {

namespace {

/// futureId as the handler keys it: the session in the upper 32 bits, the wire bits below.
std::uint64_t localFutureId(SessionId session, std::uint64_t wireOrFull) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(session)) << 32) | (wireOrFull & 0xFFFFFFFFull);
}

} // namespace

//------------------------------------------------------------------------------
// ServerSettings / ServerPlayerState / ServerPlayers
//------------------------------------------------------------------------------

std::string ServerSettings::makeServerKey(int serverId, std::string_view key) {
    if (key.empty()) return {};
    // Java String.trim: strips chars <= ' ' from both ends
    std::size_t b = 0, e = key.size();
    while (b < e && static_cast<unsigned char>(key[b]) <= ' ') ++b;
    while (e > b && static_cast<unsigned char>(key[e - 1]) <= ' ') --e;
    const std::string raw = std::to_string(serverId) + "_" + std::string(key.substr(b, e - b));
    std::string       out;
    for (const char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 && u < 128) out.push_back(c);
        else if (c == '-' || c == '_') out.push_back(c);
        else if (c == ' ') out.push_back('_');
    }
    return out;
}

ServerPlayerState::ServerPlayerState(
    SessionId             session,
    std::string           peerName,
    ITransport&           transport,
    BandwidthLimiter&     shared,
    const ServerSettings& settings
)
: session_(session),
  peerName_(std::move(peerName)),
  settings_(settings),
  sender(transport, session, [this] { return sessionConfig().playerBandwidthLimit; }, shared) {}

void ServerPlayerState::setPosition(std::string levelName, BlockPos2D position) {
    std::lock_guard lock(mutex_);
    levelName_ = std::move(levelName);
    position_  = position;
}
BlockPos2D ServerPlayerState::position() const {
    std::lock_guard lock(mutex_);
    return position_;
}
std::string ServerPlayerState::levelName() const {
    std::lock_guard lock(mutex_);
    return levelName_;
}
void ServerPlayerState::setClientConfig(const net::SessionConfigValues& values) {
    std::lock_guard lock(mutex_);
    clientConfig_ = values;
}
bool ServerPlayerState::isReady() const {
    std::lock_guard lock(mutex_);
    return clientConfig_.has_value();
}
net::SessionConfigValues ServerPlayerState::sessionConfig() const {
    std::optional<net::SessionConfigValues> client;
    {
        std::lock_guard lock(mutex_);
        client = clientConfig_;
    }
    if (!client) return settings_.values;
    return net::constrain(*client, settings_.values);
}

ServerPlayers::ServerPlayers(ITransport& transport, const IClock& clock, ServerSettings s)
: settings(std::move(s)),
  transport_(transport),
  clock_(clock),
  bandwidth_([this] { return settings.globalBandwidthLimit; }) {}

std::shared_ptr<ServerPlayerState> ServerPlayers::add(SessionId session, std::string peerName) {
    auto state = std::make_shared<ServerPlayerState>(session, std::move(peerName), transport_, bandwidth_, settings);
    std::lock_guard lock(mutex_);
    players_[session] = state;
    return state;
}

void ServerPlayers::remove(SessionId session) {
    std::shared_ptr<ServerPlayerState> state;
    std::vector<LeaveListener>         listeners;
    {
        std::lock_guard lock(mutex_);
        auto            it = players_.find(session);
        if (it == players_.end()) return;
        state = std::move(it->second);
        players_.erase(it);
        for (auto& [id, l] : leaveListeners_) listeners.push_back(l);
    }
    for (auto& l : listeners) l(session);
    state->sender.close();
}

std::shared_ptr<ServerPlayerState> ServerPlayers::find(SessionId session) const {
    std::lock_guard lock(mutex_);
    auto            it = players_.find(session);
    return it == players_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<ServerPlayerState>> ServerPlayers::all() const {
    std::lock_guard                                 lock(mutex_);
    std::vector<std::shared_ptr<ServerPlayerState>> out;
    for (const auto& [id, p] : players_) out.push_back(p);
    return out;
}

std::vector<std::shared_ptr<ServerPlayerState>> ServerPlayers::readyPlayers() const {
    auto out = all();
    out.erase(std::remove_if(out.begin(), out.end(), [](const auto& p) { return !p->isReady(); }), out.end());
    return out;
}

void ServerPlayers::setPosition(SessionId session, std::string levelName, BlockPos2D position) {
    if (auto p = find(session)) p->setPosition(std::move(levelName), position);
}

void ServerPlayers::tickSenders() {
    // one share for the whole pass: senders finishing mid-pass must not enlarge the others' share
    const int share = bandwidth_.bandwidthShare();
    for (const auto& p : all()) p->sender.tick(share);
}

std::uint64_t ServerPlayers::addLeaveListener(LeaveListener l) {
    std::lock_guard lock(mutex_);
    const auto      id  = nextListener_++;
    leaveListeners_[id] = std::move(l);
    return id;
}
void ServerPlayers::removeLeaveListener(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    leaveListeners_.erase(id);
}

//------------------------------------------------------------------------------
// ServerRequestHandler internals
//------------------------------------------------------------------------------

struct ServerRequestHandler::Limiters {
    SupplierBasedRateAndConcurrencyLimiter<std::uint64_t> generation;
    SupplierBasedRateAndConcurrencyLimiter<std::uint64_t> syncOnLoad;

    Limiters(ServerRequestHandler& h, SessionId session, const IClock& clock)
    : generation(
          [&h] { return h.players_.settings.values.generationRequestRateLimit; },
          [&h, session](const std::uint64_t& id) {
              h.sendException(
                  session,
                  id,
                  net::RemoteException::RateLimited,
                  "Full data request rate limit: " + std::to_string(h.constrainedGenLimit(session))
              );
          },
          clock
      ),
      syncOnLoad(
          [&h] { return h.players_.settings.values.syncOnLoadRateLimit; },
          [&h, session](const std::uint64_t& id) {
              h.sendException(
                  session,
                  id,
                  net::RemoteException::RateLimited,
                  "Sync on login rate limit: " + std::to_string(h.constrainedSyncLimit(session))
              );
          },
          clock
      ) {}
};

struct ServerRequestHandler::Request {
    SessionId                 session  = 0;
    std::uint64_t             futureId = 0;
    std::uint64_t             pos      = 0;
    bool                      sync     = false;
    std::shared_ptr<Limiters> limiters;
    std::atomic<bool>         released{false};
    std::atomic<bool>         cancelled{false};
};

struct ServerRequestHandler::Group {
    std::uint64_t                                     pos = 0;
    std::map<std::uint64_t, std::shared_ptr<Request>> requests; ///< by futureId (ascending = deterministic order)
    std::shared_ptr<const FullDataSource>             source;   ///< set once fully generated data was loaded
    bool                                              worldGenComplete = false;
    bool                                              closed           = false;
    int                                               retries          = 0;
    std::int64_t                                      firstRetryNanos  = 0;
};

ServerRequestHandler::ServerRequestHandler(
    ServerPlayers&     players,
    GeneratedProvider& provider,
    IExecutor&         work,
    LevelInfo          level,
    BeaconSupplier     beacons,
    CodecOptions       codec
)
: players_(players),
  provider_(provider),
  work_(work),
  level_(std::move(level)),
  beacons_(std::move(beacons)),
  codec_(codec) {
    std::weak_ptr<bool> weak = alive_;
    updateListener_ = provider_.addDeltaListener([this, weak](const std::shared_ptr<const FullDataSource>& delta) {
        if (auto a = weak.lock()) onSectionUpdated(delta);
    });
    leaveListener_  = players_.addLeaveListener([this, weak](SessionId s) {
        if (auto a = weak.lock()) onPlayerLeft(s);
    });
}

ServerRequestHandler::~ServerRequestHandler() { close(); }

void ServerRequestHandler::close() {
    if (closed_.exchange(true)) return;
    provider_.removeDeltaListener(updateListener_);
    players_.removeLeaveListener(leaveListener_);

    std::vector<std::shared_ptr<Request>> all;
    {
        std::lock_guard lock(mutex_);
        for (auto& [pos, g] : groupsByPos_) {
            g->closed = true;
            for (auto& [id, r] : g->requests) all.push_back(r);
        }
        for (auto& [id, r] : syncRequests_) {
            r->cancelled = true;
            all.push_back(r);
        }
        groupsByPos_.clear();
        groupsByFutureId_.clear();
        syncRequests_.clear();
    }
    for (auto& r : all) release(*r);
    alive_.reset(); // pending callbacks stop touching this object
}

void ServerRequestHandler::release(Request& request) {
    if (request.released.exchange(true)) return;
    if (request.sync) request.limiters->syncOnLoad.release();
    else request.limiters->generation.release();
    activeTickets_.fetch_sub(1);
}

std::shared_ptr<ServerRequestHandler::Limiters> ServerRequestHandler::limitersFor(SessionId session) {
    std::lock_guard lock(mutex_);
    auto&           slot = limiters_[session];
    if (!slot) slot = std::make_shared<Limiters>(*this, session, players_.clock());
    return slot;
}

bool ServerRequestHandler::sendMessage(SessionId session, const net::Message& message) {
    return players_.transport().send(session, net::encodeFrame(message));
}

void ServerRequestHandler::sendException(
    SessionId            session,
    std::uint64_t        futureId,
    net::RemoteException kind,
    const std::string&   message
) {
    net::ExceptionMessage m;
    m.futureId = futureId;
    m.kind     = kind;
    m.message  = message;
    sendMessage(session, m);
    std::lock_guard lock(stats_mutex_);
    ++stats_.exceptionsSent;
}

void ServerRequestHandler::sendResponse(
    SessionId                                     session,
    std::uint64_t                                 futureId,
    const std::shared_ptr<const FullDataPayload>& payload
) {
    net::FullDataSourceResponseMessage m;
    m.futureId = futureId;
    if (payload) m.payload = payload->header();
    sendMessage(session, m);
    std::lock_guard lock(stats_mutex_);
    ++stats_.responsesSent;
}

std::vector<net::BeaconBeam> ServerRequestHandler::beaconsFor(std::uint64_t pos) const {
    return beacons_ ? beacons_(pos) : std::vector<net::BeaconBeam>{};
}

std::int32_t ServerRequestHandler::distanceInChunks(std::uint64_t pos, const ServerPlayerState& player) const {
    const BlockPos2D p = player.position();
    return sec::chebyshevSignedDistance(pos, p.x, p.z) / 16;
}

int ServerRequestHandler::constrainedGenLimit(SessionId session) const {
    auto p = players_.find(session);
    return p ? p->sessionConfig().generationRequestRateLimit : players_.settings.values.generationRequestRateLimit;
}
int ServerRequestHandler::constrainedSyncLimit(SessionId session) const {
    auto p = players_.find(session);
    return p ? p->sessionConfig().syncOnLoadRateLimit : players_.settings.values.syncOnLoadRateLimit;
}

ServerHandlerStats ServerRequestHandler::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

std::size_t ServerRequestHandler::groupCount() const {
    std::lock_guard lock(mutex_);
    return groupsByPos_.size();
}

net::SessionConfigValues ServerRequestHandler::configMessageFor(const ServerPlayerState& player) const {
    net::SessionConfigValues cfg = player.sessionConfig();
    // ServerPlayerState.sendConfigMessage: the generation centre is divided by the dimension's coordinate scale
    const double scale = level_.coordinateScale;
    if (scale != 0.0) {
        cfg.generationCenterChunkX =
            satcast_i32(static_cast<double>(players_.settings.values.generationCenterChunkX) / scale);
        cfg.generationCenterChunkZ =
            satcast_i32(static_cast<double>(players_.settings.values.generationCenterChunkZ) / scale);
    }
    return cfg;
}

//------------------------------------------------------------------------------
// request entry (AbstractDhServerLevel.registerNetworkHandlers)
//------------------------------------------------------------------------------

void ServerRequestHandler::onRequest(SessionId session, const net::FullDataSourceRequestMessage& request) {
    if (closed_.load()) return;
    if (request.levelName != level_.levelName) return; // not this level (ILevelRelatedMessage.isSameLevelAs)
    auto player = players_.find(session);
    if (!player) return;

    const std::uint64_t             futureId = localFutureId(session, request.futureId);
    const std::uint64_t             pos      = static_cast<std::uint64_t>(request.sectionPos);
    const std::int32_t              distance = distanceInChunks(pos, *player);
    const net::SessionConfigValues& cfg      = players_.settings.values;

    if (!request.clientTimestamp.has_value()) {
        if (distance > cfg.maxGenerationRequestDistance) {
            sendException(
                session,
                futureId,
                net::RemoteException::RequestOutOfRange,
                "Distance too large: " + std::to_string(distance) + " > "
                    + std::to_string(cfg.maxGenerationRequestDistance)
            );
            return;
        }
        if (!isPosInWorldGenRange(
                pos,
                cfg.generationCenterChunkX,
                cfg.generationCenterChunkZ,
                cfg.generationMaxChunkRadius
            )) {
            sendException(session, futureId, net::RemoteException::RequestOutOfRange, "Section out of allowed bounds");
            return;
        }
        const auto plan = static_cast<GeneratorPlan>(cfg.generatorPlan);
        if (!surfaceGenEnabled(plan) && sec::detail(pos) > sec::SECTION_BLOCK_DETAIL_LEVEL) {
            sendException(
                session,
                futureId,
                net::RemoteException::SectionRequiresSplitting,
                "Only full chunks are supported by the server generator plan"
            );
            return;
        }
        queueWorldGen(player, futureId, pos);
    } else {
        if (distance > cfg.maxSyncOnLoadRequestDistance) {
            sendException(
                session,
                futureId,
                net::RemoteException::RequestOutOfRange,
                "Distance too large: " + std::to_string(distance) + " > "
                    + std::to_string(cfg.maxSyncOnLoadRequestDistance)
            );
            return;
        }
        queueSync(player, futureId, pos, *request.clientTimestamp);
    }
}

//------------------------------------------------------------------------------
// generation requests
//------------------------------------------------------------------------------

void ServerRequestHandler::queueWorldGen(
    const std::shared_ptr<ServerPlayerState>& player,
    std::uint64_t                             futureId,
    std::uint64_t                             pos
) {
    const auto plan = static_cast<GeneratorPlan>(players_.settings.values.generatorPlan);
    if (!generationEnabled(plan)) {
        sendException(
            player->session(),
            futureId,
            net::RemoteException::RequestRejected,
            "Operation is disabled in config."
        );
        return;
    }

    auto limiters = limitersFor(player->session());
    if (!limiters->generation.tryAcquire(futureId)) return; // RateLimited already sent

    auto request      = std::make_shared<Request>();
    request->session  = player->session();
    request->futureId = futureId;
    request->pos      = pos;
    request->sync     = false;
    request->limiters = std::move(limiters);
    activeTickets_.fetch_add(1);
    enqueueRequest(request);
}

void ServerRequestHandler::enqueueRequest(const std::shared_ptr<Request>& request) {
    std::shared_ptr<Group> group;
    bool                   created   = false;
    bool                   closed    = false;
    bool                   duplicate = false;
    {
        std::lock_guard lock(mutex_);
        if (closed_.load()) {
            closed = true;
        } else {
            if (groupsByFutureId_.count(request->futureId) != 0 || syncRequests_.count(request->futureId) != 0) {
                duplicate = true;
            }
        }
        if (!closed && !duplicate) {
            auto& slot = groupsByPos_[request->pos];
            if (!slot) {
                slot      = std::make_shared<Group>();
                slot->pos = request->pos;
                created   = true;
            }
            group                                = slot;
            group->requests[request->futureId]   = request;
            groupsByFutureId_[request->futureId] = group;
        }
    }
    if (closed || duplicate) {
        release(*request);
        if (duplicate)
            sendException(
                request->session,
                request->futureId,
                net::RemoteException::RequestRejected,
                "Duplicate request id"
            );
        return;
    }
    if (created) tryFulfill(group);
}

void ServerRequestHandler::tryFulfill(const std::shared_ptr<Group>& group) {
    std::weak_ptr<bool> weak = alive_;
    provider_.getAsync(group->pos, [this, weak, group](std::shared_ptr<const FullDataSource> source) {
        auto a = weak.lock();
        if (!a) return;
        onGroupSourceLoaded(group, std::move(source));
    });
}

void ServerRequestHandler::onGroupSourceLoaded(
    const std::shared_ptr<Group>&         group,
    std::shared_ptr<const FullDataSource> source
) {
    {
        std::lock_guard lock(mutex_);
        if (group->closed) return;
    }
    if (!source) {
        failGroup(group, net::RemoteException::RequestRejected, "Unable to load the requested section");
        return;
    }

    if (GeneratedProvider::generationStepsAreFullyGenerated(source->columnGenerationStep)) {
        std::lock_guard lock(mutex_);
        if (!group->closed) group->source = std::move(source);
        return;
    }

    // FullDataSourceRequestHandler :217-221
    const std::int8_t minDetail =
        level_.internalServerGenerator ? sec::SECTION_MINIMUM_DETAIL_LEVEL : provider_.lowestDataDetailLevel();
    if (sec::detail(group->pos) > minDetail) {
        failGroup(group, net::RemoteException::SectionRequiresSplitting, "Section requires splitting");
        return;
    }

    bool complete;
    int  retries = 0;
    {
        std::lock_guard lock(mutex_);
        complete = group->worldGenComplete;
        if (complete) {
            ++group->retries;
            if (group->firstRetryNanos == 0) group->firstRetryNanos = players_.clock().steadyNanos();
            retries = static_cast<int>((players_.clock().steadyNanos() - group->firstRetryNanos) / 1'000'000);
            // `retries` is now the milliseconds spent retrying
        }
    }
    if (complete) {
        // generation reported done but the data is still incomplete: look again (Java: unbounded)
        if (retries > kFulfillRetryWindowMs) {
            failGroup(group, net::RemoteException::RequestRejected, "Section generation did not complete");
        } else {
            tryFulfill(group);
        }
        return;
    }

    std::shared_ptr<RetrievalFuture> future = provider_.queueRetrieval(group->pos);
    if (!future) {
        failGroup(group, net::RemoteException::RequestRejected, "World generation is unavailable");
        return;
    }
    std::weak_ptr<bool> weak = alive_;
    future->onComplete([this, weak, group](const RetrievalResult& r) {
        auto a = weak.lock();
        if (!a) return;
        switch (r.kind) {
        case RetrievalResult::Kind::Success:
            break; // onWorldGenTaskComplete drives the fulfilment
        case RetrievalResult::Kind::Split:
            failGroup(group, net::RemoteException::SectionRequiresSplitting, "Section requires splitting");
            break;
        case RetrievalResult::Kind::Rejected:
        case RetrievalResult::Kind::Failed:
            failGroup(group, net::RemoteException::RequestRejected, r.error.empty() ? "Generation failed" : r.error);
            break;
        }
    });
}

void ServerRequestHandler::failGroup(
    const std::shared_ptr<Group>& group,
    net::RemoteException          kind,
    const std::string&            message
) {
    std::vector<std::shared_ptr<Request>> requests;
    {
        std::lock_guard lock(mutex_);
        if (group->closed) return;
        group->closed = true;
        auto it       = groupsByPos_.find(group->pos);
        if (it != groupsByPos_.end() && it->second == group) groupsByPos_.erase(it);
        for (auto& [id, r] : group->requests) {
            groupsByFutureId_.erase(id);
            requests.push_back(r);
        }
        group->requests.clear();
    }
    rejectAll(std::move(requests), kind, message);
}

void ServerRequestHandler::rejectAll(
    std::vector<std::shared_ptr<Request>> requests,
    net::RemoteException                  kind,
    const std::string&                    message
) {
    for (auto& r : requests) {
        release(*r);
        sendException(r->session, r->futureId, kind, message);
    }
}

void ServerRequestHandler::cancel(SessionId session, std::uint64_t futureIdBits) {
    const std::uint64_t id = localFutureId(session, futureIdBits);

    std::shared_ptr<Request> removed;
    bool                     groupEmptied = false;
    std::uint64_t            groupPos     = 0;
    {
        std::lock_guard lock(mutex_);
        if (auto s = syncRequests_.find(id); s != syncRequests_.end()) {
            removed            = s->second;
            removed->cancelled = true;
            syncRequests_.erase(s);
        } else if (auto g = groupsByFutureId_.find(id); g != groupsByFutureId_.end()) {
            std::shared_ptr<Group> group = g->second;
            groupsByFutureId_.erase(g);
            auto rit = group->requests.find(id);
            if (rit != group->requests.end()) {
                removed = rit->second;
                group->requests.erase(rit);
            }
            if (group->requests.empty() && !group->closed) {
                group->closed = true;
                groupPos      = group->pos;
                auto pit      = groupsByPos_.find(group->pos);
                if (pit != groupsByPos_.end() && pit->second == group) groupsByPos_.erase(pit);
                groupEmptied = true;
            }
        }
    }
    if (removed) {
        removed->cancelled = true;
        release(*removed);
    }
    if (groupEmptied) {
        provider_.removeRetrievalRequestIf([groupPos](std::uint64_t p) { return p == groupPos; });
    }
}

void ServerRequestHandler::onWorldGenTaskComplete(std::uint64_t pos) {
    std::shared_ptr<Group> group;
    {
        std::lock_guard lock(mutex_);
        auto            it = groupsByPos_.find(pos);
        if (it == groupsByPos_.end()) return;
        group                   = it->second;
        group->worldGenComplete = true;
    }
    tryFulfill(group);
}

void ServerRequestHandler::tick() {
    struct Ready {
        std::shared_ptr<Group>                group;
        std::shared_ptr<const FullDataSource> source;
        std::vector<std::shared_ptr<Request>> requests;
    };
    std::vector<Ready> ready;
    {
        std::lock_guard lock(mutex_);
        for (auto it = groupsByPos_.begin(); it != groupsByPos_.end();) { // ascending pos
            const std::shared_ptr<Group>& g = it->second;
            if (!g->source || g->closed) {
                ++it;
                continue;
            }
            g->closed = true; // unavailable for adding into
            Ready r;
            r.group  = g;
            r.source = g->source;
            for (auto& [id, req] : g->requests) {
                groupsByFutureId_.erase(id);
                r.requests.push_back(req);
            }
            g->requests.clear();
            ready.push_back(std::move(r));
            it = groupsByPos_.erase(it);
        }
    }

    for (Ready& r : ready) {
        auto                requests = std::make_shared<std::vector<std::shared_ptr<Request>>>(std::move(r.requests));
        auto                source   = r.source;
        const std::uint64_t pos      = r.group->pos;
        std::weak_ptr<bool> weak     = alive_;
        const bool          accepted = work_.execute([this, weak, requests, source, pos] {
            auto a = weak.lock();
            if (!a) return;
            auto made = makePayload(*source, beaconsFor(pos), players_.bufferIds().allocate(), codec_);
            if (!made) {
                rejectAll(*requests, net::RemoteException::RequestRejected, "Unable to encode the data source");
                return;
            }
            auto payload = std::make_shared<const FullDataPayload>(std::move(*made));
            {
                std::lock_guard lock(stats_mutex_);
                ++stats_.payloadsEncoded;
            }
            for (auto& req : *requests) deliver(req, payload);
        });
        if (!accepted) rejectAll(*requests, net::RemoteException::RequestRejected, "Server is shutting down");
    }
}

void ServerRequestHandler::deliver(
    const std::shared_ptr<Request>&               request,
    const std::shared_ptr<const FullDataPayload>& payload
) {
    auto player = players_.find(request->session);
    if (!player || request->cancelled.load()) {
        release(*request);
        return;
    }
    std::weak_ptr<bool> weak = alive_;
    player->sender.sendInChunks(
        payload,
        [this, weak, request, payload] {
            auto a = weak.lock();
            if (!a) return;
            sendResponse(request->session, request->futureId, payload);
            release(*request);
        },
        [this, weak, request] {
            auto a = weak.lock();
            if (!a) return;
            release(*request);
        }
    );
}

//------------------------------------------------------------------------------
// sync-on-load requests
//------------------------------------------------------------------------------

void ServerRequestHandler::queueSync(
    const std::shared_ptr<ServerPlayerState>& player,
    std::uint64_t                             futureId,
    std::uint64_t                             pos,
    std::int64_t                              clientTimestamp
) {
    if (!player->sessionConfig().synchronizeOnLoad) {
        sendException(
            player->session(),
            futureId,
            net::RemoteException::RequestRejected,
            "Operation is disabled in config."
        );
        return;
    }

    auto limiters = limitersFor(player->session());
    if (!limiters->syncOnLoad.tryAcquire(futureId)) return; // RateLimited already sent

    auto request      = std::make_shared<Request>();
    request->session  = player->session();
    request->futureId = futureId;
    request->pos      = pos;
    request->sync     = true;
    request->limiters = std::move(limiters);
    activeTickets_.fetch_add(1);
    bool duplicate = false;
    {
        std::lock_guard lock(mutex_);
        if (closed_.load()) {
            // released below
        } else if (groupsByFutureId_.count(futureId) != 0 || syncRequests_.count(futureId) != 0) {
            duplicate = true;
        } else {
            syncRequests_[futureId] = request;
        }
    }
    if (duplicate) {
        release(*request);
        sendException(request->session, futureId, net::RemoteException::RequestRejected, "Duplicate request id");
        return;
    }
    if (closed_.load()) {
        release(*request);
        return;
    }

    std::weak_ptr<bool> weak     = alive_;
    const bool          accepted = work_.execute([this, weak, request, clientTimestamp] {
        auto a = weak.lock();
        if (!a) return;
        runSync(request, clientTimestamp);
    });
    if (!accepted) {
        {
            std::lock_guard lock(mutex_);
            syncRequests_.erase(futureId);
        }
        release(*request);
        sendException(request->session, futureId, net::RemoteException::RequestRejected, "Server is shutting down");
    }
}

void ServerRequestHandler::runSync(const std::shared_ptr<Request>& request, std::int64_t clientTimestamp) {
    auto finish = [this, &request] {
        {
            std::lock_guard lock(mutex_);
            syncRequests_.erase(request->futureId);
        }
        release(*request);
    };
    if (request->cancelled.load() || closed_.load()) {
        finish();
        return;
    }

    // FullDataSourceRequestHandler :90-104
    const std::optional<std::int64_t> serverTimestamp = provider_.timestamp(request->pos);
    if (!serverTimestamp.has_value() || *serverTimestamp <= clientTimestamp) {
        // either no data exists to sync, or the client is already up to date
        finish();
        sendResponse(request->session, request->futureId, nullptr);
        return;
    }

    std::shared_ptr<const FullDataSource> source = provider_.get(request->pos);
    if (!source) {
        finish();
        sendException(
            request->session,
            request->futureId,
            net::RemoteException::RequestRejected,
            "Unable to read the section"
        );
        return;
    }
    auto made = makePayload(*source, beaconsFor(request->pos), players_.bufferIds().allocate(), codec_);
    if (!made) {
        finish();
        sendException(
            request->session,
            request->futureId,
            net::RemoteException::RequestRejected,
            "Unable to encode the data source"
        );
        return;
    }
    auto payload = std::make_shared<const FullDataPayload>(std::move(*made));
    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.payloadsEncoded;
    }

    auto player = players_.find(request->session);
    if (!player || request->cancelled.load()) {
        finish();
        return;
    }
    std::weak_ptr<bool> weak = alive_;
    player->sender.sendInChunks(
        payload,
        [this, weak, request, payload] {
            auto a = weak.lock();
            if (!a) return;
            sendResponse(request->session, request->futureId, payload);
            {
                std::lock_guard lock(mutex_);
                syncRequests_.erase(request->futureId);
            }
            release(*request);
        },
        [this, weak, request] {
            auto a = weak.lock();
            if (!a) return;
            {
                std::lock_guard lock(mutex_);
                syncRequests_.erase(request->futureId);
            }
            release(*request);
        }
    );
}

//------------------------------------------------------------------------------
// real-time updates (AbstractDhServerLevel.updateDataSourcesAsync)
//------------------------------------------------------------------------------

void ServerRequestHandler::onSectionUpdated(std::shared_ptr<const FullDataSource> delta) {
    if (closed_.load() || !delta || !players_.settings.values.enableRealTimeUpdates) return;
    std::weak_ptr<bool> weak = alive_;
    work_.execute([this, weak, delta = std::move(delta)] {
        const std::uint64_t pos = delta->pos;
        auto                a   = weak.lock();
        if (!a) return;

        std::vector<std::shared_ptr<ServerPlayerState>> targets;
        for (auto& p : players_.readyPlayers()) {
            if (p->levelName() != level_.levelName) continue;
            const net::SessionConfigValues cfg = p->sessionConfig();
            if (!cfg.enableRealTimeUpdates) continue;
            if (distanceInChunks(pos, *p) <= cfg.realTimeUpdateDistanceRadius) targets.push_back(p);
        }
        if (targets.empty()) return;

        // Java sends only the incoming delta (updateDataSourcesAsync(data)), not the merged section
        auto made = makePayload(*delta, beaconsFor(pos), players_.bufferIds().allocate(), codec_);
        if (!made) return;
        auto payload = std::make_shared<const FullDataPayload>(std::move(*made));
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.payloadsEncoded;
        }
        for (auto& p : targets) {
            const SessionId session = p->session();
            p->sender.sendInChunks(payload, [this, weak, session, payload] {
                auto a2 = weak.lock();
                if (!a2) return;
                net::FullDataPartialUpdateMessage m;
                m.levelName = level_.levelName;
                m.payload   = payload->header();
                sendMessage(session, m);
                std::lock_guard lock(stats_mutex_);
                ++stats_.realtimeUpdatesSent;
            });
        }
    });
}

//------------------------------------------------------------------------------
// player leaving
//------------------------------------------------------------------------------

void ServerRequestHandler::onPlayerLeft(SessionId session) {
    std::vector<std::shared_ptr<Request>> dropped;
    std::vector<std::uint64_t>            emptiedPositions;
    {
        std::lock_guard lock(mutex_);
        for (auto it = groupsByPos_.begin(); it != groupsByPos_.end();) {
            Group& g = *it->second;
            for (auto rit = g.requests.begin(); rit != g.requests.end();) {
                if (rit->second->session == session) {
                    groupsByFutureId_.erase(rit->first);
                    dropped.push_back(rit->second);
                    rit = g.requests.erase(rit);
                } else {
                    ++rit;
                }
            }
            if (g.requests.empty() && !g.closed) {
                g.closed = true;
                emptiedPositions.push_back(g.pos);
                it = groupsByPos_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = syncRequests_.begin(); it != syncRequests_.end();) {
            if (it->second->session == session) {
                it->second->cancelled = true;
                dropped.push_back(it->second);
                it = syncRequests_.erase(it);
            } else {
                ++it;
            }
        }
        limiters_.erase(session);
    }
    for (auto& r : dropped) {
        r->cancelled = true;
        release(*r);
    }
    for (std::uint64_t p : emptiedPositions) {
        provider_.removeRetrievalRequestIf([p](std::uint64_t q) { return q == p; });
    }
}

//------------------------------------------------------------------------------
// ServerNetwork
//------------------------------------------------------------------------------

ServerNetwork::ServerNetwork(ServerPlayers& players) : players_(players) { players_.transport().setListener(this); }

ServerNetwork::~ServerNetwork() { players_.transport().setListener(nullptr); }

void ServerNetwork::addLevel(ServerRequestHandler& handler) {
    std::lock_guard lock(mutex_);
    levels_.push_back(&handler);
}

void ServerNetwork::removeLevel(ServerRequestHandler& handler) {
    std::lock_guard lock(mutex_);
    levels_.erase(std::remove(levels_.begin(), levels_.end(), &handler), levels_.end());
}

ServerRequestHandler* ServerNetwork::findLevelByName(const std::string& name) const {
    std::lock_guard lock(mutex_);
    for (auto* l : levels_)
        if (l->level().levelName == name) return l;
    return nullptr;
}

ServerRequestHandler* ServerNetwork::findLevelByDimension(const std::string& dimension) const {
    std::lock_guard lock(mutex_);
    for (auto* l : levels_)
        if (l->level().dimension == dimension) return l;
    return nullptr;
}

void ServerNetwork::broadcastConfig() {
    for (const auto& p : players_.readyPlayers()) sendConfig(*p);
}

void ServerNetwork::tick() {
    std::vector<ServerRequestHandler*> levels;
    {
        std::lock_guard lock(mutex_);
        levels = levels_;
    }
    for (auto* l : levels) l->tick();
    players_.tickSenders();
}

void ServerNetwork::onConnected(SessionId session, std::string_view peerName) {
    players_.add(session, std::string(peerName));
}

void ServerNetwork::onDisconnected(SessionId session) { players_.remove(session); }

void ServerNetwork::sendConfig(const ServerPlayerState& player) {
    ServerRequestHandler*     level = findLevelByName(player.levelName());
    net::SessionConfigMessage m;
    if (level != nullptr) m.config = level->configMessageFor(player);
    else m.config = player.sessionConfig();
    players_.transport().send(player.session(), net::encodeFrame(m));
}

void ServerNetwork::onFrame(SessionId session, std::span<const std::byte> frame) {
    auto player = players_.find(session);
    if (!player) return;

    auto decoded = net::decodeFrame(frame);
    if (!decoded) {
        if (decoded.error().code == ErrorCode::Unsupported) {
            // Client won't understand this message, but it is enough to display an incompatible-protocol error
            net::CloseReasonMessage m;
            m.reason = "Incompatible protocol version";
            players_.transport().send(session, net::encodeFrame(m));
            players_.transport().disconnect(session);
        } else {
            corrupt_.fetch_add(1);
        }
        return;
    }

    net::Message& message = **decoded;
    switch (message.id()) {
    case net::MsgIds::SessionConfig: {
        player->setClientConfig(static_cast<net::SessionConfigMessage&>(message).config);
        sendConfig(*player);
        break;
    }
    case net::MsgIds::RequestLevelInit: {
        if (!players_.settings.sendLevelKeys) break;
        const auto&           req   = static_cast<net::RequestLevelInitMessage&>(message);
        ServerRequestHandler* level = findLevelByDimension(req.dimensionResourceLocation);
        if (level == nullptr) break;
        net::LevelInitMessage m;
        m.dimensionResourceLocation = req.dimensionResourceLocation;
        m.serverKey                 = players_.settings.serverKey;
        m.levelKey                  = level->level().levelKey;
        m.serverTime                = players_.clock().nowMillis();
        players_.transport().send(session, net::encodeFrame(m));
        break;
    }
    case net::MsgIds::FullDataSourceRequest: {
        auto&                 req   = static_cast<net::FullDataSourceRequestMessage&>(message);
        ServerRequestHandler* level = findLevelByName(req.levelName);
        if (level == nullptr) {
            net::ExceptionMessage m;
            m.futureId = req.futureId;
            m.kind     = net::RemoteException::RequestRejected;
            m.message  = "Unknown level";
            players_.transport().send(session, net::encodeFrame(m));
            break;
        }
        level->onRequest(session, req);
        break;
    }
    case net::MsgIds::Cancel: {
        const auto&                        cancel = static_cast<net::CancelMessage&>(message);
        std::vector<ServerRequestHandler*> levels;
        {
            std::lock_guard lock(mutex_);
            levels = levels_;
        }
        for (auto* l : levels) l->cancel(session, cancel.futureId);
        break;
    }
    case net::MsgIds::CloseReason:
        players_.transport().disconnect(session);
        break;
    default:
        break; // server-to-client messages are ignored
    }
}

} // namespace dh::mp
