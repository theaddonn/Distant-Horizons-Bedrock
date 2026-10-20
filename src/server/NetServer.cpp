#include "server/NetServer.h"

#include "bedrock_common/FileLogger.h"
#include "bedrock_common/LevelNaming.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
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
    // One naming rule shared with the client driver: see LevelNaming.h.
    li.dimension       = dimensionResource;
    li.levelKey        = makeLevelKey(options.server.levelKeyPrefix, worldName, dimensionResource);
    li.levelName       = makeLevelName(options.server.sendLevelKeys, li.levelKey, dimensionResource);
    li.coordinateScale = coordinateScale;
    li.internalServerGenerator =
        options.common.worldGenerator.chunkGeneratorMode == dh::DistantGeneratorMode::InternalServer;
    return li;
}

NetServer::NetServer(PacketTransport& transport, dh::IClock& clock, dh::ConfigStore& config)
: transport_(transport),
  config_(config),
  players_(transport, clock, settingsFrom(config.options())),
  network_(players_) {
    generationAllowed_.store(players_.settings.values.generatorPlan != dh::net::GeneratorPlan::Disabled);
}

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
            if (joinHook_) joinHook_();
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
    logEffectiveSettings();

    // Players already online (hot enable, late start): they never produce a PlayerJoinEvent for us.
    try {
        if (auto level = ll::service::getLevel()) {
            level->forEachPlayer([this](Player& p) -> bool {
                if (p.isSimulated()) return true;
                const auto id = transport_.openSession(p.getNetworkIdentifier(), p.getClientSubId(), p.getRealName());
                DH_LOG_I(
                    "net: player '%s' already online, session %llu",
                    p.getRealName().c_str(),
                    (unsigned long long)id
                );
                return true;
            });
        }
    } catch (...) {
        DH_LOG_E("net: exception swallowed while opening sessions for online players");
    }
}

void NetServer::logEffectiveSettings() const {
    const auto& v = players_.settings.values;
    // Bytes per 50 ms tick for one sender alone (Payload.cpp: rate*1000/20 + 1, 0 = unlimited).
    auto perTick = [](int kbps) -> long long { return kbps <= 0 ? 0LL : (long long)kbps * 1000 / 20 + 1; };
    DH_LOG_I(
        "net: effective server settings: playerBandwidthLimit=%d KB/s (%lld B/tick) globalBandwidthLimit=%d KB/s "
        "(%lld B/tick) generationRequestRateLimit=%d syncOnLoadRateLimit=%d maxGenerationRequestDistance=%d "
        "maxSyncOnLoadRequestDistance=%d generatorPlan=%d chunkGeneratorMode=%d enableRealTimeUpdates=%d "
        "sendLevelKeys=%d frameBytes=%zu (0 B/tick = unlimited)",
        v.playerBandwidthLimit,
        perTick(v.playerBandwidthLimit),
        players_.settings.globalBandwidthLimit,
        perTick(players_.settings.globalBandwidthLimit),
        v.generationRequestRateLimit,
        v.syncOnLoadRateLimit,
        v.maxGenerationRequestDistance,
        v.maxSyncOnLoadRequestDistance,
        (int)v.generatorPlan,
        (int)config_.options().common.worldGenerator.chunkGeneratorMode,
        v.enableRealTimeUpdates ? 1 : 0,
        players_.settings.sendLevelKeys ? 1 : 0,
        transport_.maxFrameBytes()
    );
}

void NetServer::onPlayerLevelChanged(dh::SessionId session) {
    transport_.runOnDispatch([this, session] { network_.resendConfig(session); });
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
        dh::mp::ServerSettings next = settingsFrom(config_.options());
        generationAllowed_.store(next.values.generatorPlan != dh::net::GeneratorPlan::Disabled);
        // ServerNetwork::onFrame reads players_.settings on the transport dispatch thread: write it there too.
        transport_.runOnDispatch([this, next = std::move(next)]() mutable {
            players_.settings = std::move(next);
            network_.broadcastConfig();
            logEffectiveSettings();
            DH_LOG_I("net: config changed, session config re-sent to %zu player(s)", players_.readyPlayers().size());
        });
    } catch (...) {
        DH_LOG_E("net: exception swallowed while applying a config change");
    }
}

} // namespace dh::bedrock
