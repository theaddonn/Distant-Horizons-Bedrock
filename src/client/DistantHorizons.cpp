#include "client/DistantHorizons.h"

#include "bedrock_common/FileLogger.h"
#include "bedrock_common/LeviLogger.h"
#include "client/Version.h"
#include "client/hooks/ChunkIngress.h"
#include "client/hud/DebugHud.h"
#include "client/spikes/Spikes.h"

#include "dh/Version.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/mod/RegisterHelper.h"

namespace distant_horizons {

namespace {
ll::event::ListenerPtr joinListener;
ll::event::ListenerPtr exitListener;
} // namespace

DistantHorizons& DistantHorizons::getInstance() {
    static DistantHorizons instance;
    return instance;
}

bool DistantHorizons::load() {
    dh::bedrock::installCoreLogger();
    DH_LOG_I("DH loaded v%s build %s (core %s)", dh::kModVersion, dh::kGitSha, dh::coreVersion());
    return true;
}

bool DistantHorizons::enable() {
    auto& bus    = ll::event::EventBus::getInstance();
    joinListener = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) { DH_LOG_I("level: join"); });
    exitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { DH_LOG_I("level: exit"); });
    dh::hud::install();
    dh::spikes::install();
    dh::ingress::install();
    DH_LOG_I("DH enabled");
    return true;
}

bool DistantHorizons::disable() {
    dh::ingress::uninstall();
    dh::spikes::uninstall();
    dh::hud::uninstall();
    auto& bus = ll::event::EventBus::getInstance();
    if (joinListener) bus.removeListener(joinListener);
    if (exitListener) bus.removeListener(exitListener);
    joinListener.reset();
    exitListener.reset();
    DH_LOG_I("DH disabled");
    return true;
}

} // namespace distant_horizons

LL_REGISTER_MOD(distant_horizons::DistantHorizons, distant_horizons::DistantHorizons::getInstance());
