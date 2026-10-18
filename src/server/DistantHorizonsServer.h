#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

#include <memory>
#include <string>

namespace dh::bedrock {
class ServerWorldDriver;
}

namespace distant_horizons::server {

class DistantHorizonsServer {

public:
    static DistantHorizonsServer& getInstance();

    DistantHorizonsServer();
    ~DistantHorizonsServer();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    /// One-line driver status for `/dh status`.
    [[nodiscard]] std::string statusText() const;

private:
    void startDriver();
    void stopDriver();
    void registerCommand();

    ll::mod::NativeMod&                             mSelf;
    std::unique_ptr<dh::bedrock::ServerWorldDriver> mDriver;
    ll::event::ListenerPtr                          mStartedListener;
    ll::event::ListenerPtr                          mStoppingListener;
    ll::event::ListenerPtr                          mTickListener;
    bool                                            mCommandRegistered = false;
};

} // namespace distant_horizons::server
