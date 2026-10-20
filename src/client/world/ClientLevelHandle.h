#pragma once

// dh::ILevelHandle over a Bedrock Dimension (client side).
//
// One class serves both roles; `clientSide` decides:
//   * client handle  (isClientSide()==true):  wraps the dimension of the ClientLevel; owns the TableClientLevel that
//     colours LODs; serverSideHandle() returns the paired server handle in singleplayer, nullptr when remote.
//   * server handle  (isClientSide()==false): wraps the dimension of the in-process server Level (singleplayer only).
//
// Identity (docs/PORTING.md, LevelNaming.h):
//   singleplayer  dhIdentifier() = dimension resource location, for BOTH handles (they must agree: the store folder
//                 and the shared DhClientServerLevel are keyed by it).
//   remote        dhIdentifier() = the server's levelKey once setLevelKey() was called with the LevelInit answer
//                 (then isServerKeyed()==true), else the dimension resource location (server with sendLevelKeys off,
//                 or the answer has not arrived yet). setLevelKey() is ignored once lockIdentity() was called (the
//                 world keys its entries by the identifier, so it must not change while the level is loaded).
// The identifier storage is append-only so a string_view handed out earlier stays valid for the handle's lifetime.
//
// Everything the core reads (info()) is copied at construction: the core threads never touch live game objects.
// The Dimension pointer is only dereferenced by game-thread / owner-thread glue (chunk fetching, generator).

#include "dh/platform/BlockTable.h"
#include "dh/platform/TableClientLevel.h"
#include "dh/world/LevelHandle.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

class Dimension;

namespace dh::bedrock {

class ClientLevelHandle final : public dh::ILevelHandle {
public:
    ClientLevelHandle(
        Dimension&      dimension,
        int             dimensionId,
        dh::LevelInfo   info,
        dh::BlockTable& table,
        bool            clientSide
    );

    // ILevelHandle
    std::string_view      dhIdentifier() const override;
    const dh::ILevelInfo& info() const override { return level_; }
    bool                  isClientSide() const override { return clientSide_; }
    dh::IClientLevel*     clientLevel() override { return clientSide_ ? &level_ : nullptr; }
    dh::ILevelHandle*     serverSideHandle() override { return paired_; }
    std::string           levelKey() const override;
    double                coordinateScale() const override { return dimensionId_ == 1 ? 8.0 : 1.0; }
    bool                  isServerKeyed() const override { return keyed_.load(); }

    /// Links the client handle to the server handle of the same dimension (singleplayer).
    void setPaired(dh::ILevelHandle* server) { paired_ = server; }

    /// Network layer: the server answered LevelInit for this dimension. Any thread. Ignored when locked/empty.
    void setLevelKey(const std::string& levelKey);
    /// Called by the driver once the level is loaded: the identifier never changes afterwards.
    void lockIdentity() { locked_.store(true); }
    /// A (late) LevelInit key that arrived while locked is parked here; the driver applies it between unload and reload.
    bool hasPendingKey() const;
    /// Unlocks and applies the parked key. Only call with the level unloaded.
    bool applyPendingKey();

    Dimension*           dimension() const { return dimension_; }
    int                  dimensionId() const { return dimensionId_; }
    const dh::LevelInfo& rawInfo() const { return raw_; }
    const std::string&   dimensionResource() const { return raw_.dimensionName; }

private:
    Dimension*           dimension_;
    int                  dimensionId_;
    bool                 clientSide_;
    dh::LevelInfo        raw_;
    dh::TableClientLevel level_;
    dh::ILevelHandle*    paired_ = nullptr;

    mutable std::mutex              mutex_;
    std::deque<std::string>         identifiers_; ///< append-only; back() is current
    std::atomic<const std::string*> current_{nullptr};
    std::string                     levelKey_;
    std::string                     pendingKey_;
    std::atomic<bool>               keyed_{false};
    std::atomic<bool>               locked_{false};
};

} // namespace dh::bedrock
