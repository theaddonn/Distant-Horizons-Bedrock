#include "dh/world/World.h"

#include <algorithm>
#include <string>
#include <utility>

namespace dh {

//=========//
// helpers //
//=========//

net::SessionConfigValues sessionValuesFromOptions(const Options& o) {
    net::SessionConfigValues v;
    v.generatorPlan = static_cast<net::GeneratorPlan>(static_cast<std::uint8_t>(o.common.worldGenerator.generatorPlan));
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
    return v;
}

mp::ServerSettings serverSettingsFromOptions(const Options& o) {
    mp::ServerSettings s;
    s.values               = sessionValuesFromOptions(o);
    s.globalBandwidthLimit = o.server.globalBandwidthLimit;
    s.sendLevelKeys        = o.server.sendLevelKeys;
    s.serverKey            = mp::ServerSettings::makeServerKey(o.server.serverId, o.server.serverKey);
    return s;
}

//=============//
// AbstractWorld //
//=============//

AbstractWorld::AbstractWorld(WorldEnvironment environment, const WorldDeps& deps)
: deps_(deps),
  environment_(environment) {}

AbstractWorld::~AbstractWorld() = default; // concrete destructors call close()

const char* AbstractWorld::environmentName() const {
    switch (environment_) {
    case WorldEnvironment::ClientOnly:
        return "CLIENT_ONLY";
    case WorldEnvironment::ClientServer:
        return "CLIENT_SERVER";
    case WorldEnvironment::ServerOnly:
        return "SERVER_ONLY";
    }
    return "?";
}

bool AbstractWorld::applyChunkUpdate(
    ILevelHandle&                        handle,
    std::shared_ptr<IChunk>              chunk,
    std::vector<std::shared_ptr<IChunk>> neighbours
) {
    AbstractLevel* level = getLevel(handle);
    if (level == nullptr) return false;
    return level->updateChunkAsync(std::move(chunk), std::move(neighbours));
}

void AbstractWorld::tick() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
    }
    tickImpl();
}

void AbstractWorld::onOptionsChanged() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
    }
    for (AbstractLevel* level : allLoadedLevels()) level->onOptionsChanged();
    onOptionsChangedImpl();
}

void AbstractWorld::close() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        closed_ = true;
    }
    closeImpl();
    deps_.log().log(LogLevel::Info, std::string("Closed DhWorld of type [") + environmentName() + "].");
}

std::string AbstractWorld::debugString() {
    return std::string(environmentName()) + " World with " + std::to_string(loadedLevelCount()) + " levels";
}

void AbstractWorld::reportLoadFailure(const char* kind, const std::string& id, const std::string& detail) {
    bool first;
    {
        std::lock_guard lock(mutex_);
        first = reportedFailures_.insert(id).second;
    }
    if (!first) return;
    // The one user-facing message (Java ClientApi.queueSlowChatMessage). The platform's ILogger decides where it shows.
    deps_.log().log(
        LogLevel::Error,
        std::string("Distant Horizons: ") + kind + " level loading failed. Unable to load level [" + id
            + "], LODs may not appear. See log for more information. (" + detail + ")"
    );
}

void AbstractWorld::clearLoadFailure(const std::string& id) {
    std::lock_guard lock(mutex_);
    reportedFailures_.erase(id);
}

void AbstractWorld::tickServerNetwork() {
    if (deps_.serverNetwork != nullptr && deps_.tickServerNetwork) deps_.serverNetwork->tick();
}

//===============//
// DhClientWorld //
//===============//

DhClientWorld::DhClientWorld(const WorldDeps& deps) : AbstractWorld(WorldEnvironment::ClientOnly, deps) {
    deps_.log().log(LogLevel::Info, "Started DhWorld of type CLIENT_ONLY");
    if (deps.clientTransport != nullptr && deps.clock != nullptr && deps.registry != nullptr
        && deps.options != nullptr) {
        mp::ClientNetConfig cfg;
        cfg.wanted                = sessionValuesFromOptions(*deps.options);
        cfg.adaptiveTransferSpeed = deps.options->server.enableAdaptiveTransferSpeed;
        net_ = std::make_unique<mp::ClientNetworkState>(*deps.clientTransport, *deps.clock, *deps.registry, cfg);
        net_->setWorkExecutor(deps.exec.networkWork);
        if (net_->isConnected()) net_->sendConfigMessage(); // Java: networkState.sendConfigMessage() in the constructor
    }
}

DhClientWorld::~DhClientWorld() { close(); }

DhClientWorld::Entry* DhClientWorld::find(const std::string& id) {
    for (Entry& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

DhClientLevel* DhClientWorld::getOrLoadLevel(ILevelHandle& handle) {
    if (!handle.isClientSide()) return nullptr; // DhClientWorld.java: !(wrapper instanceof IClientLevelWrapper)

    const std::string              id = std::string(handle.dhIdentifier());
    std::shared_ptr<DhClientLevel> stale;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return nullptr;
        if (Entry* e = find(id)) {
            if (e->handle == &handle) return e->level.get();
            // storedLevel.getClientLevelWrapper() != wrapper: unload the old wrapper's level, create a new one
            unloadLocked(*e->handle, stale);
        }
    }
    if (stale) {
        stale->close();
        stale.reset();
    }

    // create outside the world lock: the store factory / level construction is platform code
    std::shared_ptr<DhClientLevel> level = createClientLevel(handle);
    if (!level) return nullptr;
    std::lock_guard lock(mutex_);
    if (closed_) return nullptr;
    if (Entry* e = find(id)) return e->level.get(); // lost a race with another loader
    entries_.push_back(Entry{id, &handle, level, {&handle}});
    return level.get();
}

bool DhClientWorld::ensureLevelKeyWhenAvailable(ILevelHandle& handle) {
    if (net_ == nullptr) return true; // no DH server to ask
    bool sendInit;
    {
        std::lock_guard lock(mutex_);
        // Java sendLevelInitRequestIfNeed: once per client level object
        sendInit = levelInitRequested_.insert(&handle).second;
    }
    if (sendInit) net_->sendLevelInitRequest(std::string(handle.info().dimensionName()));
    if (handle.isServerKeyed()) return true;

    // Make non-keyed levels wait some delay since the first attempt to load anything, so the server can reply to the
    // level key request (DhClientWorld.java ensureLevelKeyWhenAvailable, FIRST_LEVEL_LOAD_DELAY_IN_MS)
    const std::int64_t now = deps_.clock->nowMillis();
    std::lock_guard    lock(mutex_);
    if (allowLoadingLevelsAfterMs_ == 0) allowLoadingLevelsAfterMs_ = now + kFirstLevelLoadDelayMs;
    return now >= allowLoadingLevelsAfterMs_;
}

std::shared_ptr<DhClientLevel> DhClientWorld::createClientLevel(ILevelHandle& handle) {
    const std::string id = std::string(handle.dhIdentifier());
    if (!ensureLevelKeyWhenAvailable(handle)) return nullptr; // not a failure: the caller retries later

    auto level = std::make_shared<DhClientLevel>(handle, deps_, net_.get());
    if (!level->ok()) {
        const std::string detail = level->error();
        level.reset();
        reportLoadFailure("Client", id, detail);
        return nullptr;
    }
    clearLoadFailure(id);
    return level;
}

bool DhClientWorld::unloadLocked(ILevelHandle& handle, std::shared_ptr<DhClientLevel>& toClose) {
    const std::string id = std::string(handle.dhIdentifier());
    Entry*            e  = find(id);
    if (e == nullptr) return false;
    deps_.log().log(LogLevel::Info, "Unloading level [" + id + "].");
    handle.onUnload();
    e->wrappers.erase(&handle);
    levelInitRequested_.erase(&handle);
    if (e->wrappers.empty()) {
        toClose = std::move(e->level);
        entries_.erase(entries_.begin() + (e - entries_.data()));
    }
    return true;
}

bool DhClientWorld::unloadLevel(ILevelHandle& handle) {
    if (!handle.isClientSide()) return false;
    std::shared_ptr<DhClientLevel> toClose;
    bool                           known;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        known = unloadLocked(handle, toClose);
    }
    if (toClose) toClose->close(); // drains executors: never under the world lock
    return known;
}

DhClientLevel* DhClientWorld::getLevel(ILevelHandle& handle) {
    if (!handle.isClientSide()) return nullptr;
    std::lock_guard lock(mutex_);
    Entry*          e = find(std::string(handle.dhIdentifier()));
    return e ? e->level.get() : nullptr;
}

std::vector<AbstractLevel*> DhClientWorld::allLoadedLevels() {
    std::lock_guard             lock(mutex_);
    std::vector<AbstractLevel*> out;
    for (Entry& e : entries_) out.push_back(e.level.get());
    return out;
}

void DhClientWorld::tickImpl() {
    std::vector<std::shared_ptr<DhClientLevel>> levels;
    {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_) levels.push_back(e.level);
    }
    for (auto& l : levels) l->tick();
}

void DhClientWorld::onOptionsChangedImpl() {
    // Java configAnyChangeListener: the wanted session values changed -> re-send
    if (net_ != nullptr && deps_.options != nullptr) {
        net_->setWanted(sessionValuesFromOptions(*deps_.options));
        net_->setAdaptiveTransferSpeed(deps_.options->server.enableAdaptiveTransferSpeed);
    }
}

void DhClientWorld::closeImpl() {
    std::vector<Entry> entries;
    {
        std::lock_guard lock(mutex_);
        entries.swap(entries_);
        levelInitRequested_.clear();
    }
    for (Entry& e : entries) {
        for (ILevelHandle* w : e.wrappers) w->onUnload();
        e.level->close();
    }
    entries.clear();
    net_.reset(); // detaches the transport listener; after the levels so their queues can cancel cleanly
}

//=====================//
// DhClientServerWorld //
//=====================//

DhClientServerWorld::DhClientServerWorld(const WorldDeps& deps) : AbstractWorld(WorldEnvironment::ClientServer, deps) {
    deps_.log().log(LogLevel::Info, "Started DhWorld of type CLIENT_SERVER");
}

DhClientServerWorld::~DhClientServerWorld() { close(); }

DhClientServerWorld::Entry* DhClientServerWorld::findByServer(ILevelHandle* serverHandle) {
    for (Entry& e : entries_) {
        if (e.serverHandle == serverHandle) return &e;
    }
    return nullptr;
}

DhClientServerWorld::Entry* DhClientServerWorld::findByClient(ILevelHandle* clientHandle) {
    for (Entry& e : entries_) {
        if (std::find(e.clientWrappers.begin(), e.clientWrappers.end(), clientHandle) != e.clientWrappers.end())
            return &e;
    }
    return nullptr;
}

DhClientServerLevel* DhClientServerWorld::getOrLoadLevel(ILevelHandle& handle) {
    if (!handle.isClientSide()) {
        // server level wrapper: create the shared level (DhClientServerWorld.java:76-107)
        const std::string id = std::string(handle.dhIdentifier());
        {
            std::lock_guard lock(mutex_);
            if (closed_) return nullptr;
            if (Entry* e = findByServer(&handle)) return e->level.get();
        }
        auto level = std::make_shared<DhClientServerLevel>(handle, deps_);
        if (!level->ok()) {
            const std::string detail = level->error();
            level.reset();
            reportLoadFailure("ClientServer", id, detail);
            return nullptr;
        }
        clearLoadFailure(id);
        std::lock_guard lock(mutex_);
        if (closed_) return nullptr;
        if (Entry* e = findByServer(&handle)) return e->level.get();
        entries_.push_back(Entry{&handle, level, {}});
        return level.get();
    }

    // client level wrapper: attach to the existing server level (DhClientServerWorld.java:110-141)
    ILevelHandle* serverHandle = handle.serverSideHandle();
    if (serverHandle == nullptr) {
        deps_.log().log(
            LogLevel::Error,
            "tryGetServerSideWrapper returned no server level for client level [" + std::string(handle.dhIdentifier())
                + "]."
        );
        return nullptr;
    }
    if (serverHandle->info().dimensionName() != handle.info().dimensionName()) {
        deps_.log().log(
            LogLevel::Error,
            "tryGetServerSideWrapper returned a level for a different dimension. Client: ["
                + std::string(handle.dhIdentifier()) + "] Server: [" + std::string(serverHandle->dhIdentifier()) + "]."
        );
        return nullptr;
    }
    std::shared_ptr<DhClientServerLevel> level;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return nullptr;
        Entry* e = findByServer(serverHandle);
        if (e == nullptr) return nullptr; // the server level is not loaded (yet)
        level = e->level;
        if (std::find(e->clientWrappers.begin(), e->clientWrappers.end(), &handle) == e->clientWrappers.end()) {
            e->clientWrappers.push_back(&handle);
        }
        level->startRenderer(handle); // idempotent; registers this handle as a viewer (under the lock, see unloadLevel)
    }
    return level.get();
}

bool DhClientServerWorld::unloadLevel(ILevelHandle& handle) {
    std::shared_ptr<DhClientServerLevel> toClose;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        if (!handle.isClientSide()) {
            Entry* e = findByServer(&handle);
            if (e == nullptr) return false;
            deps_.log().log(LogLevel::Info, "Unloading level [" + std::string(handle.dhIdentifier()) + "]");
            handle.onUnload();
            for (ILevelHandle* w : e->clientWrappers) w->onUnload(); // attached client handles notice the level is gone
            toClose = std::move(e->level);
            entries_.erase(entries_.begin() + (e - entries_.data()));
        } else {
            // The client side leaves the level but the server side still has it loaded: do not unload it, just stop
            // rendering once no wrapper is left (DhClientServerWorld.java:150-173).
            Entry* e = findByClient(&handle);
            if (e == nullptr) return false;
            e->clientWrappers.erase(
                std::remove(e->clientWrappers.begin(), e->clientWrappers.end(), &handle),
                e->clientWrappers.end()
            );
            e->level->forgetClientHandle(handle);
            // stop under the world lock: a concurrent getOrLoadLevel(client) cannot slip in between check and stop
            if (e->clientWrappers.empty() && e->level->isRendering()) e->level->stopRenderer();
            handle.onUnload(); // we still want to unload the wrapper though
        }
    }
    if (toClose) toClose->close();
    return true;
}

DhClientServerLevel* DhClientServerWorld::getLevel(ILevelHandle& handle) {
    std::lock_guard lock(mutex_);
    Entry*          e = handle.isClientSide() ? findByClient(&handle) : findByServer(&handle);
    return e ? e->level.get() : nullptr;
}

std::size_t DhClientServerWorld::clientWrapperCount(ILevelHandle& serverHandle) {
    std::lock_guard lock(mutex_);
    Entry*          e = findByServer(&serverHandle);
    return e ? e->clientWrappers.size() : 0;
}

std::vector<AbstractLevel*> DhClientServerWorld::allLoadedLevels() {
    std::lock_guard             lock(mutex_);
    std::vector<AbstractLevel*> out;
    for (Entry& e : entries_) out.push_back(e.level.get());
    return out;
}

void DhClientServerWorld::tickImpl() {
    std::vector<std::shared_ptr<DhClientServerLevel>> levels;
    {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_) levels.push_back(e.level);
    }
    for (auto& l : levels) l->tick();
    tickServerNetwork();
}

void DhClientServerWorld::onOptionsChangedImpl() {
    if (deps_.serverNetwork != nullptr) deps_.serverNetwork->broadcastConfig();
}

void DhClientServerWorld::closeImpl() {
    std::vector<Entry> entries;
    {
        std::lock_guard lock(mutex_);
        entries.swap(entries_);
    }
    for (Entry& e : entries) {
        e.serverHandle->onUnload();
        for (ILevelHandle* w : e.clientWrappers) w->onUnload();
        e.level->close();
    }
}

//===============//
// DhServerWorld //
//===============//

DhServerWorld::DhServerWorld(const WorldDeps& deps) : AbstractWorld(WorldEnvironment::ServerOnly, deps) {
    deps_.log().log(LogLevel::Info, "Started DhServerWorld of type SERVER_ONLY");
}

DhServerWorld::~DhServerWorld() { close(); }

DhServerWorld::Entry* DhServerWorld::find(ILevelHandle* handle) {
    for (Entry& e : entries_) {
        if (e.handle == handle) return &e;
    }
    return nullptr;
}

DhServerLevel* DhServerWorld::getOrLoadLevel(ILevelHandle& handle) {
    if (handle.isClientSide()) return nullptr; // DhServerWorld.java: !(wrapper instanceof IServerLevelWrapper)
    const std::string id = std::string(handle.dhIdentifier());
    {
        std::lock_guard lock(mutex_);
        if (closed_) return nullptr;
        if (Entry* e = find(&handle)) return e->level.get();
    }
    auto level = std::make_shared<DhServerLevel>(handle, deps_);
    if (!level->ok()) {
        const std::string detail = level->error();
        level.reset();
        reportLoadFailure("Server", id, detail);
        return nullptr;
    }
    clearLoadFailure(id);
    std::lock_guard lock(mutex_);
    if (closed_) return nullptr;
    if (Entry* e = find(&handle)) return e->level.get();
    entries_.push_back(Entry{&handle, level});
    return level.get();
}

bool DhServerWorld::unloadLevel(ILevelHandle& handle) {
    if (handle.isClientSide()) return false;
    std::shared_ptr<DhServerLevel> toClose;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        Entry* e = find(&handle);
        if (e == nullptr) return false;
        handle.onUnload();
        toClose = std::move(e->level);
        entries_.erase(entries_.begin() + (e - entries_.data()));
    }
    toClose->close();
    return true;
}

DhServerLevel* DhServerWorld::getLevel(ILevelHandle& handle) {
    std::lock_guard lock(mutex_);
    Entry*          e = find(&handle);
    return e ? e->level.get() : nullptr;
}

std::vector<AbstractLevel*> DhServerWorld::allLoadedLevels() {
    std::lock_guard             lock(mutex_);
    std::vector<AbstractLevel*> out;
    for (Entry& e : entries_) out.push_back(e.level.get());
    return out;
}

void DhServerWorld::setPlayerPosition(SessionId session, ILevelHandle& level, BlockPos2D position) {
    if (deps_.serverPlayers != nullptr)
        deps_.serverPlayers->setPosition(session, std::string(level.dhIdentifier()), position);
}

void DhServerWorld::tickImpl() {
    std::vector<std::shared_ptr<DhServerLevel>> levels;
    {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_) levels.push_back(e.level);
    }
    for (auto& l : levels) l->tick();
    tickServerNetwork();
}

void DhServerWorld::onOptionsChangedImpl() {
    if (deps_.serverNetwork != nullptr) deps_.serverNetwork->broadcastConfig();
}

void DhServerWorld::closeImpl() {
    std::vector<Entry> entries;
    {
        std::lock_guard lock(mutex_);
        entries.swap(entries_);
    }
    for (Entry& e : entries) {
        e.handle->onUnload();
        e.level->close();
    }
}

} // namespace dh
