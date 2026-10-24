#include "server/DistantHorizonsServer.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "bedrock_common/GenProbe.h"
#include "bedrock_common/LeviLogger.h"
#include "bedrock_common/Platform.h"
#include "server/ServerWorldDriver.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

namespace distant_horizons::server {

DistantHorizonsServer& DistantHorizonsServer::getInstance() {
    static DistantHorizonsServer instance;
    return instance;
}

DistantHorizonsServer::DistantHorizonsServer() : mSelf(*ll::mod::NativeMod::current()) {}

DistantHorizonsServer::~DistantHorizonsServer() = default;

bool DistantHorizonsServer::load() {
    dh::bedrock::installCoreLogger(); // own log file (<modDir>/logs/dh.log) + LeviLamina logger
    DH_LOG_I("server: loading");
    return true;
}

void DistantHorizonsServer::startDriver() {
    try {
        if (mDriver) return;
        auto level = ll::service::getLevel();
        if (!level) {
            DH_LOG_W("server: no Level yet, driver not started");
            return;
        }
        auto driver = std::make_unique<dh::bedrock::ServerWorldDriver>(
            *level,
            dh::bedrock::ModPaths::fromCurrentMod(),
            getSelf().getModDir()
        );
        if (!driver->start()) {
            DH_LOG_E("server: driver failed to start (see above); Distant Horizons stays inactive");
            return;
        }
        mDriver = std::move(driver);
    } catch (...) {
        DH_LOG_E("server: exception while starting the driver");
    }
}

void DistantHorizonsServer::stopDriver() {
    try {
        if (!mDriver) return;
        mDriver->stop();
        mDriver.reset();
    } catch (...) {
        DH_LOG_E("server: exception while stopping the driver");
    }
}

std::string DistantHorizonsServer::statusText() const {
    if (!mDriver || !mDriver->running()) return "Distant Horizons: inactive";
    return "Distant Horizons: " + mDriver->statusText();
}

void DistantHorizonsServer::registerCommand() {
    if (mCommandRegistered) return;
    try {
        auto& cmd = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
            "dh",
            "Distant Horizons server status",
            CommandPermissionLevel::GameDirectors
        );
        cmd.overload().text("status").execute([this](CommandOrigin const&, CommandOutput& output) {
            output.success("{}", statusText());
        });
        cmd.overload().text("sweep").execute([this](CommandOrigin const&, CommandOutput& output) {
            if (mDriver) mDriver->sweepAll();
            output.success("Distant Horizons: loaded chunks re-queued (see the log for the counts)");
        });
        mCommandRegistered = true;
    } catch (...) {
        DH_LOG_W("server: could not register /dh status");
    }
}

bool DistantHorizonsServer::enable() {
    try {
        DH_LOG_I("server: enabling (git %s)", DH_GIT_SHA);
        const auto paths = dh::bedrock::ModPaths::fromCurrentMod();
        dh::bedrock::ConfigBridge::instance().init(paths.configDir);

        auto& bus        = ll::event::EventBus::getInstance();
        mStartedListener = bus.emplaceListener<ll::event::ServerStartedEvent>([this](ll::event::ServerStartedEvent&) {
            startDriver();
        });
        mStoppingListener =
            bus.emplaceListener<ll::event::ServerStoppingEvent>([this](ll::event::ServerStoppingEvent&) {
                stopDriver();
            });
        mTickListener = bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](ll::event::ServerLevelTickEvent&) {
            if (mDriver) mDriver->onServerTick();
        });
        registerCommand();
        dh::bedrock::probe::start(); // no-op unless DH_GEN_PROBE=1

        // Hot enable while the server is already running.
        if (ll::service::getLevel()) startDriver();
        return true;
    } catch (...) {
        DH_LOG_E("server: exception while enabling");
        return false;
    }
}

bool DistantHorizonsServer::disable() {
    try {
        DH_LOG_I("server: disabling");
        auto& bus = ll::event::EventBus::getInstance();
        if (mTickListener) bus.removeListener(mTickListener);
        if (mStartedListener) bus.removeListener(mStartedListener);
        if (mStoppingListener) bus.removeListener(mStoppingListener);
        mTickListener.reset();
        mStartedListener.reset();
        mStoppingListener.reset();
        stopDriver();
        dh::bedrock::probe::stop();
        return true;
    } catch (...) {
        DH_LOG_E("server: exception while disabling");
        return false;
    }
}

} // namespace distant_horizons::server

LL_REGISTER_MOD(
    distant_horizons::server::DistantHorizonsServer,
    distant_horizons::server::DistantHorizonsServer::getInstance()
);
