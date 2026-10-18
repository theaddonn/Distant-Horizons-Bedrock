#include "server/NetServer.h"

#include "bedrock_common/FileLogger.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "mc/server/ServerPlayer.h"

#include <algorithm>

namespace dh::bedrock {

namespace {

dh::net::GeneratorPlan toNetPlan(dh::GeneratorPlan p) {
    switch (p) {
    case dh::GeneratorPlan::SurfaceThenChunks:
        return dh::net::GeneratorPlan::SurfaceThenChunks;
    case dh::GeneratorPlan::SurfaceOnly:
        return dh::net::GeneratorPlan::SurfaceOnly;
    case dh::GeneratorPlan::ChunksOnly:
        return dh::net::GeneratorPlan::ChunksOnly;
    case dh::GeneratorPlan::Disabled:
        return dh::net::GeneratorPlan::Disabled;
    }
    return dh::net::GeneratorPlan::SurfaceThenChunks;
}

/// [a-zA-Z0-9-_] only, at most 150 chars (K1: LevelInitMessage validation).
std::string keySafe(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 150) out.resize(150);
    return out;
}

} // namespace

dh::mp::ServerSettings NetServer::settingsFrom(const dh::Options& o) {
    dh::mp::ServerSettings s;
    auto&                  v       = s.values;
    v.generatorPlan                = toNetPlan(o.common.worldGenerator.generatorPlan);
    v.maxGenerationRequestDistance = o.server.maxGenerationRequestDistance;
    v.generationCenterChunkX       = o.common.worldGenerator.generationCenterChunkX;
    v.generationCenterChunkZ       = o.common.worldGenerator.generationCenterChunkZ;
    v.generationMaxChunkRadius     = o.common.worldGenerator.generationMaxChunkRadius;
    v.generationRequestRateLimit   = o.server.generationRequestRateLimit;
    v.enableRealTimeUpdates        = o.server.enableRealTimeUpdates;
    v.realTimeUpdateDistanceRadius = o.server.realTimeUpdateDistanceRadiusInChunks;
    v.synchronizeOnLoad            = o.server.synchronizeOnLoad;
    v.maxSyncOnLoadRequestDistance = o.server.maxSyncOnLoadRequestDistance;
    v.syncOnLoadRateLimit          = o.server.syncOnLoadRateLimit;
    v.playerBandwidthLimit         = o.server.playerBandwidthLimit;
    s.globalBandwidthLimit         = o.server.globalBandwidthLimit;
    s.sendLevelKeys                = o.server.sendLevelKeys;
    s.serverKey                    = dh::mp::ServerSettings::makeServerKey(o.server.serverId, o.server.serverKey);
    return s;
}

dh::mp::LevelInfo NetServer::levelInfoFor(
    const std::string& worldName,
    const std::string& dimensionResource,
    double             coordinateScale,
    const dh::Options& options
) {
    dh::mp::LevelInfo li;
    // levelName is what requests carry (Java getDhIdentifier): unique per world + dimension.
    li.levelName       = worldName + "@" + dimensionResource;
    li.dimension       = dimensionResource;
    li.levelKey        = keySafe(options.server.levelKeyPrefix + worldName + "_" + dimensionResource);
    li.coordinateScale = coordinateScale;
    li.internalServerGenerator =
        options.common.worldGenerator.chunkGeneratorMode == dh::DistantGeneratorMode::InternalServer;
    return li;
}

NetServer::NetServer(PacketTransport& transport, dh::IClock& clock, dh::ConfigStore& config)
: transport_(transport),
  config_(config),
  players_(transport, clock, settingsFrom(config.options())),
  network_(players_) {}

NetServer::~NetServer() { stop(); }

void NetServer::start() {
    if (started_) return;
    started_ = true;
    transport_.setListener(&network_);

    auto& bus      = ll::event::EventBus::getInstance();
    joinListener_  = bus.emplaceListener<ll::event::PlayerJoinEvent>([this](ll::event::PlayerJoinEvent& e) {
        try {
            ServerPlayer& p  = e.self();
            const auto    id = transport_.openSession(p.getNetworkIdentifier(), p.getClientSubId(), p.getRealName());
            DH_LOG_I("net: player '%s' joined, session %llu", p.getRealName().c_str(), (unsigned long long)id);
        } catch (...) {
            DH_LOG_E("net: exception swallowed in PlayerJoinEvent");
        }
    });
    leaveListener_ = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([this](ll::event::PlayerDisconnectEvent& e) {
        try {
            ServerPlayer& p = e.self();
            transport_.closeSession(p.getNetworkIdentifier());
            DH_LOG_I("net: player '%s' left", p.getRealName().c_str());
        } catch (...) {
            DH_LOG_E("net: exception swallowed in PlayerDisconnectEvent");
        }
    });

    // ConfigStore has no unsubscribe: capture only the shared flag, never `this`.
    std::shared_ptr<std::atomic<bool>> flag = configDirty_;
    config_.onChange([flag](const dh::Options&) { flag->store(true); });
    DH_LOG_I("net: server network started (max frame %zu bytes)", transport_.maxFrameBytes());
}

void NetServer::stop() {
    if (!started_) return;
    started_  = false;
    auto& bus = ll::event::EventBus::getInstance();
    if (joinListener_) bus.removeListener(joinListener_);
    if (leaveListener_) bus.removeListener(leaveListener_);
    joinListener_.reset();
    leaveListener_.reset();
    transport_.setListener(nullptr);
    configDirty_->store(false);
}

void NetServer::applyPendingConfig() {
    if (!configDirty_->exchange(false)) return;
    try {
        players_.settings = settingsFrom(config_.options());
        network_.broadcastConfig();
        DH_LOG_I("net: config changed, session config re-sent to %zu player(s)", players_.readyPlayers().size());
    } catch (...) {
        DH_LOG_E("net: exception swallowed while applying a config change");
    }
}

} // namespace dh::bedrock
