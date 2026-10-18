#include "server/DistantHorizonsServer.h"

#include "ll/api/mod/RegisterHelper.h"

namespace distant_horizons::server {

DistantHorizonsServer& DistantHorizonsServer::getInstance() {
    static DistantHorizonsServer instance;
    return instance;
}

bool DistantHorizonsServer::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool DistantHorizonsServer::enable() {
    getSelf().getLogger().debug("Enabling...");
    // Code for enabling the mod goes here.
    return true;
}

bool DistantHorizonsServer::disable() {
    getSelf().getLogger().debug("Disabling...");
    // Code for disabling the mod goes here.
    return true;
}

} // namespace distant_horizons::server

LL_REGISTER_MOD(distant_horizons::server::DistantHorizonsServer, distant_horizons::server::DistantHorizonsServer::getInstance());
