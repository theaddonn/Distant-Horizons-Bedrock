#include "mod/DistantHorizons.h"

#include "ll/api/mod/RegisterHelper.h"

namespace distant_horizons {

DistantHorizons& DistantHorizons::getInstance() {
    static DistantHorizons instance;
    return instance;
}

bool DistantHorizons::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool DistantHorizons::enable() {
    getSelf().getLogger().debug("Enabling...");
    // Code for enabling the mod goes here.
    return true;
}

bool DistantHorizons::disable() {
    getSelf().getLogger().debug("Disabling...");
    // Code for disabling the mod goes here.
    return true;
}

} // namespace distant_horizons

LL_REGISTER_MOD(distant_horizons::DistantHorizons, distant_horizons::DistantHorizons::getInstance());
