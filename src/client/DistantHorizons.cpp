#include "client/DistantHorizons.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "bedrock_common/LeviLogger.h"
#include "bedrock_common/Platform.h"
#include "client/Version.h"
#include "client/hud/DebugHud.h"
#include "client/render/MaterialInjector.h"
#include "client/render/RenderHooks.h"
#include "client/world/ClientWorldDriver.h"

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
    // As early as possible: the compiled-material index is read during engine start-up, before enable() runs
    // (the first trace build installed its file hooks ~6 s after load() and never saw materials.index.json).
    dh::render::installMaterialInjector();
    return true;
}

bool DistantHorizons::enable() {
    // The render / HUD install code reads the options, so load config.json first (idempotent: the world driver calls
    // init again later).
    dh::bedrock::ConfigBridge::instance().init(dh::bedrock::ModPaths::fromCurrentMod().configDir);
    auto& bus    = ll::event::EventBus::getInstance();
    joinListener = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) { DH_LOG_I("level: join"); });
    exitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { DH_LOG_I("level: exit"); });
    dh::hud::install();
    dh::render::installMaterialInjector(); // no-op if load() already installed it
    dh::render::install();
    dh::bedrock::ClientWorldDriver::instance().install();
    DH_LOG_I("DH enabled");
    return true;
}

bool DistantHorizons::disable() {
    dh::bedrock::ClientWorldDriver::instance().uninstall();
    dh::render::uninstall();
    dh::render::uninstallMaterialInjector();
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
