#pragma once

// The platform's identity for one loaded level / dimension (Java ILevelWrapper, IClientLevelWrapper,
// IServerLevelWrapper). The glue owns the handle and keeps it alive from getOrLoadLevel() until
// unloadLevel() has returned (Java kept the wrappers alive the same way: the level held them).
//
// Handles are compared by address, never by value: in a singleplayer world the client handle and the server
// handle of the same dimension are two different objects that share one DH level (see DhClientServerWorld).

#include <string>
#include <string_view>

#include "dh/platform/ILevelInfo.h"

namespace dh {

struct ILevelHandle {
    virtual ~ILevelHandle() = default;

    /// Java getDhIdentifier(): stable name used for save folders and as the wire `levelName`
    /// (FullDataSourceRequest.levelName). Client and server handle of one dimension must agree.
    virtual std::string_view dhIdentifier() const = 0;

    virtual const ILevelInfo& info() const = 0;

    /// true for an IClientLevelWrapper (rendering side), false for an IServerLevelWrapper.
    virtual bool isClientSide() const = 0;

    /// The colour/shade provider of a client handle; nullptr for server handles.
    virtual IClientLevel* clientLevel() { return nullptr; }

    /// Java IClientLevelWrapper.tryGetServerSideWrapper(): the in-process server handle of this dimension, or
    /// nullptr when the client is connected to a remote server.
    virtual ILevelHandle* serverSideHandle() { return nullptr; }

    /// Java getKeyedLevelDimensionName(): the server's level key (LevelInitMessage); defaults to the identifier.
    virtual std::string levelKey() const { return std::string(dhIdentifier()); }

    /// Dimension coordinate scale used by the server for the generation centre (nether = 8.0).
    virtual double coordinateScale() const { return 1.0; }

    /// Java IServerKeyedClientLevel: the server already told the client which level this is, so loading is
    /// not delayed waiting for a LevelInit answer.
    virtual bool isServerKeyed() const { return false; }

    /// Java wrapper.onUnload(): called by the world once the level has been unloaded (or the world closed).
    virtual void onUnload() {}
};

} // namespace dh
