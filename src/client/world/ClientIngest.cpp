// Chunk ingestion for the client world driver (the internal porting notes, ChunkHashStore.h recipe; the server counterpart is
// src/server/ServerWorldDriver.cpp).
//
//   hooks / events   only NOTE (dimension, chunk, due time) in Session::pending (any thread, microseconds).
//                    Level::$onChunkLoaded      authoritative Level (singleplayer: the SERVER Level)
//                    ClientLevel::$onSubChunkLoaded   remote: sub-chunks trickle in, every event re-arms a debounce
//                    DBChunkStorage::$saveLiveChunk   singleplayer: dirty server chunk was saved ("block changed")
//                    BlockChangedEvent                block edits on the authoritative Level
//   drain            runs on the thread that OWNS the chunks (singleplayer: the server thread through
//                    ll::thread::ServerThreadExecutor; remote: the client game thread inside the tick): re-fetch the
//                    chunk (shared_ptr keeps it alive), BedrockChunk::snapshot() it and its loaded neighbours, skip
//                    unchanged hashes, then hand INDEPENDENT copies to world.applyChunkUpdate. lightChunk writes its
//                    seeds into every chunk of the 3x3, so a snapshot shared between two queued updates would race;
//                    each update therefore gets its own copies (a snapshot copy is a few vector memcpys).
//                    Back-pressure: at most kMaxInFlight chunk updates are queued in the level at a time.

#include "bedrock_common/BdsWorldGenerator.h"
#include "bedrock_common/BedrockChunk.h"
#include "bedrock_common/FileLogger.h"
#include "client/world/ClientDriverImpl.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/DBChunkStorage.h"

#include <algorithm>
#include <map>
#include <thread>
#include <vector>

namespace dh::bedrock {

namespace {

using namespace std::chrono_literals;

constexpr int  kMaxInFlight       = 12;  ///< queued chunk updates per level (each owns up to 9 snapshot copies)
constexpr int  kBatchChunks       = 24;  ///< chunks taken from the pending set per drain
constexpr auto kDrainBudget       = 8ms; ///< wall time per drain on the owning thread
constexpr int  kMaxNotLoadedTries = 20;
constexpr int  kRemoteDebounceMs  = 750; ///< sub-chunks arrive one by one
constexpr int  kBlockDebounceMs   = 500;
constexpr int  kRescanRadius      = 32; ///< chunks probed around the player for already-loaded chunks

bool gHooksInstalled = false;

LL_TYPE_INSTANCE_HOOK(
    ClientIngestOnChunkLoadedHook,
    ll::memory::HookPriority::Normal,
    ::Level,
    &::Level::$onChunkLoaded,
    void,
    ::ChunkSource& source,
    ::LevelChunk&  lc
) {
    origin(source, lc);
    try {
        BusyGuard g;
        auto*     t = gIngestTarget.load();
        if (!t || !t->session || this != t->session->ingestLevel) return;
        t->noteChunk(lc, t->session->singleplayer ? 0 : kRemoteDebounceMs);
    } catch (...) {
        DH_LOG_E("client ingest: exception swallowed in onChunkLoaded hook");
    }
}

LL_TYPE_INSTANCE_HOOK(
    ClientIngestOnSubChunkLoadedHook,
    ll::memory::HookPriority::Normal,
    ClientLevel,
    &ClientLevel::$onSubChunkLoaded,
    void,
    ::ChunkSource& source,
    ::LevelChunk&  lc,
    short          absoluteSubChunkIndex,
    bool           subChunkVisibilityChanged
) {
    origin(source, lc, absoluteSubChunkIndex, subChunkVisibilityChanged);
    try {
        BusyGuard g;
        auto*     t = gIngestTarget.load();
        if (!t || !t->session || t->session->singleplayer || static_cast<::Level*>(this) != t->session->ingestLevel)
            return;
        t->noteChunk(lc, kRemoteDebounceMs);
    } catch (...) {
        DH_LOG_E("client ingest: exception swallowed in onSubChunkLoaded hook");
    }
}

// DBChunkStorage is the (integrated) server's persistent chunk source: a dirty live chunk is written here, which is
// a "block changed" signal; the chunk hash makes unchanged saves free. TODO(verify in game): fires in singleplayer.
LL_TYPE_INSTANCE_HOOK(
    ClientIngestOnSaveLiveChunkHook,
    ll::memory::HookPriority::Normal,
    DBChunkStorage,
    &DBChunkStorage::$saveLiveChunk,
    bool,
    ::LevelChunk& lc
) {
    const bool r = origin(lc);
    try {
        BusyGuard g;
        auto*     t = gIngestTarget.load();
        if (!t || !t->session || !t->session->singleplayer) return r;
        t->noteChunk(lc, kBlockDebounceMs);
    } catch (...) {
        DH_LOG_E("client ingest: exception swallowed in saveLiveChunk hook");
    }
    return r;
}

} // namespace

void ClientWorldDriver::Impl::installHooks() {
    if (gHooksInstalled) return;
    gHooksInstalled = true;
    // LeviLamina convention: hook() returns 0 on success (TODO(verify in game)).
    if (ClientIngestOnChunkLoadedHook::hook() != 0) DH_LOG_E("client ingest: hooking ::Level::onChunkLoaded failed");
    if (ClientIngestOnSubChunkLoadedHook::hook() != 0)
        DH_LOG_E("client ingest: hooking ClientLevel::onSubChunkLoaded failed");
    if (ClientIngestOnSaveLiveChunkHook::hook() != 0)
        DH_LOG_W("client ingest: hooking DBChunkStorage::saveLiveChunk failed (block edits rely on BlockChangedEvent)");

    auto& bus     = ll::event::EventBus::getInstance();
    blockListener = bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& e) {
        try {
            BusyGuard g;
            auto*     t = gIngestTarget.load();
            if (!t || !t->session) return;
            if (&e.previousBlock() == &e.newBlock()) return;
            ::BlockSource& bs = e.blockSource();
            if (&bs.getLevel() != t->session->ingestLevel) return;
            const auto& pos = e.pos();
            t->noteBlock(bs.getLevel(), bs.getDimension().mId->mValue, pos.x, pos.z);
        } catch (...) {
            DH_LOG_E("client ingest: exception swallowed in BlockChangedEvent listener");
        }
    });
}

void ClientWorldDriver::Impl::uninstallHooks() {
    if (!gHooksInstalled) return;
    gHooksInstalled = false;
    gIngestTarget.store(nullptr);
    while (gBusy.load() != 0) std::this_thread::sleep_for(1ms);
    ClientIngestOnSaveLiveChunkHook::unhook();
    ClientIngestOnSubChunkLoadedHook::unhook();
    ClientIngestOnChunkLoadedHook::unhook();
    while (gBusy.load() != 0) std::this_thread::sleep_for(1ms);
}

void ClientWorldDriver::Impl::noteChunk(LevelChunk& lc, int debounceMs) {
    Session&   s = *session;
    PendingKey k;
    k.dim = lc.mDimension.mId->mValue;
    k.x   = lc.mPosition->x;
    k.z   = lc.mPosition->z;
    ++s.noted;
    const auto      due = SteadyClock::now() + std::chrono::milliseconds(debounceMs);
    std::lock_guard lock(s.pendingMutex);
    auto [it, inserted] = s.pending.try_emplace(k, PendingChunk{0, due});
    if (!inserted) it->second.due = due; // re-arm: more data arrived
}

void ClientWorldDriver::Impl::noteBlock(::Level&, int dim, int blockX, int blockZ) {
    Session&   s = *session;
    PendingKey k;
    k.dim = dim;
    k.x   = blockX >> 4;
    k.z   = blockZ >> 4;
    ++s.noted;
    const auto      due = SteadyClock::now() + std::chrono::milliseconds(kBlockDebounceMs);
    std::lock_guard lock(s.pendingMutex);
    auto [it, inserted] = s.pending.try_emplace(k, PendingChunk{0, due});
    if (!inserted) it->second.due = due;
}

void ClientWorldDriver::Impl::clearPending(Session& s) {
    std::lock_guard lock(s.pendingMutex);
    s.pending.clear();
}

void ClientWorldDriver::Impl::scheduleIngest(Session& s) {
    if (s.loadedDim.load() == kNoDimension) return;
    {
        std::lock_guard lock(s.pendingMutex);
        const auto      now = SteadyClock::now();
        bool            any = s.rescan.load();
        if (s.pending.empty() && !any) return;
        for (const auto& [k, p] : s.pending)
            if (p.due <= now) {
                any = true;
                break;
            }
        if (!any) return;
    }
    if (gDrainInFlight.exchange(true)) return;

    // The flag is cleared on EVERY exit path (also when the queued task finds no session / no target).
    struct DrainFlag {
        ~DrainFlag() {
            gDrainQueuedMs.store(0);
            gDrainInFlight.store(false);
        }
    };

    if (s.singleplayer) {
        gDrainQueuedMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count()
        );
        ll::thread::ServerThreadExecutor::getDefault().execute([] {
            DrainFlag flag;
            BusyGuard g;
            gDrainQueuedMs.store(0); // started
            auto* t = gIngestTarget.load();
            if (!t || !t->session) return;
            ++gDrains;
            try {
                t->drain(*t->session);
            } catch (...) {
                DH_LOG_E("client ingest: exception swallowed in drain");
            }
        });
    } else {
        DrainFlag flag;
        BusyGuard g;
        ++gDrains;
        try {
            drain(s);
        } catch (...) {
            DH_LOG_E("client ingest: exception swallowed in drain");
        }
    }
}

void ClientWorldDriver::Impl::drain(Session& s) {
    std::shared_lock lk(life, std::try_to_lock); // a level unload waits for us; a running unload makes us skip
    if (!lk || s.stopping.load()) return;
    const int loaded = s.loadedDim.load();
    if (loaded == kNoDimension) return;
    ClientLevelHandle* handle = s.singleplayer ? s.serverHandle(loaded) : s.clientHandle(loaded);
    if (!handle || !handle->dimension()) return;
    dh::AbstractLevel* level = s.world->getLevel(*handle);
    if (!level || level->isClosed()) return;
    Dimension& dim = *handle->dimension();

    // --- chunks that were already loaded before the level / ingestion existed ----------------------
    // There is no safe public enumeration of a ChunkSource (MainChunkSource::mChunkMap is private game state), so a
    // square around the player is probed with getAvailableChunk on this (owner) thread.
    if (s.rescan.exchange(false)) {
        const int  pcx0 = s.playerX.load() >> 4, pcz0 = s.playerZ.load() >> 4;
        const auto due   = SteadyClock::now();
        int        found = 0;
        for (int dz = -kRescanRadius; dz <= kRescanRadius; ++dz)
            for (int dx = -kRescanRadius; dx <= kRescanRadius; ++dx) {
                if (!dim.getChunkSource().getAvailableChunk(::ChunkPos(pcx0 + dx, pcz0 + dz))) continue;
                PendingKey k;
                k.dim = loaded;
                k.x   = pcx0 + dx;
                k.z   = pcz0 + dz;
                std::lock_guard lock(s.pendingMutex);
                if (s.pending.try_emplace(k, PendingChunk{0, due}).second) ++found;
            }
        s.noted += found;
        DH_LOG_I("client ingest: rescan found %d already-loaded chunks around the player", found);
    }

    // --- take due chunks, nearest to the player first -------------------------------------------
    const std::int64_t room = kMaxInFlight - level->inFlightChunkUpdates();
    if (room <= 0) return;
    const int pcx = s.playerX.load() >> 4, pcz = s.playerZ.load() >> 4;
    const int desired = s.desiredDim.load();

    struct Item {
        std::int64_t dist;
        PendingKey   key;
        int          tries;
    };
    std::vector<Item> items;
    {
        std::lock_guard lock(s.pendingMutex);
        const auto      now = SteadyClock::now();
        for (auto it = s.pending.begin(); it != s.pending.end();) {
            if (it->first.dim != loaded) {
                // Not the loaded dimension. If it is the dimension the player is entering, keep it for when that level
                // loads; anything else belongs to a level that will never load.
                if (it->first.dim != desired) {
                    ++s.skipped;
                    it = s.pending.erase(it);
                } else {
                    ++it;
                }
                continue;
            }
            if (it->second.due <= now) {
                const std::int64_t dx = it->first.x - pcx, dz = it->first.z - pcz;
                items.push_back({dx * dx + dz * dz, it->first, it->second.tries});
            }
            ++it;
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
        const std::size_t take = std::min<std::size_t>(items.size(), std::min<std::int64_t>(room, kBatchChunks));
        items.resize(take);
        for (const auto& i : items) s.pending.erase(i.key);
    }
    if (items.empty()) return;

    // --- snapshots ------------------------------------------------------------------------------
    enum class Status { Ok, NotLoaded, Missing };
    struct Snap {
        std::shared_ptr<BedrockChunk> chunk;
        Status                        status = Status::Missing;
    };
    std::map<std::pair<int, int>, Snap> cache; // this drain only: chunks may change afterwards
    auto                                snap = [&](int x, int z) -> Snap& {
        auto [it, inserted] = cache.try_emplace({x, z});
        if (!inserted) return it->second;
        Snap&                       r = it->second;
        std::shared_ptr<LevelChunk> lc = dim.getChunkSource().getAvailableChunk(::ChunkPos(x, z));
        if (!lc) return r;
        // The server only exposes complete chunks once Loaded; the client's chunk state semantics are unverified,
        // so the remote path relies on the sub-chunk debounce instead.
        if (s.singleplayer && lc->mLoadState->load() < ChunkState::Loaded) {
            r.status = Status::NotLoaded;
            return r;
        }
        if (!BedrockChunk::heightSupported(*lc)) return r;
        auto b = std::make_shared<BedrockChunk>(*lc, *s.serializer);
        b->snapshot();
        r.chunk  = std::move(b);
        r.status = Status::Ok;
        return r;
    };

    const bool skipHashes = s.config->options().common.lodBuilding.disableUnchangedChunkCheck;
    const auto deadline   = SteadyClock::now() + kDrainBudget;
    auto       requeue    = [&](const Item& i, int tries, std::chrono::milliseconds delay) {
        std::lock_guard lock(s.pendingMutex);
        s.pending.try_emplace(i.key, PendingChunk{tries, SteadyClock::now() + delay});
    };

    for (const Item& item : items) {
        if (SteadyClock::now() > deadline) {
            requeue(item, item.tries, 0ms);
            ++s.deferred;
            continue;
        }
        Snap& c = snap(item.key.x, item.key.z);
        if (c.status == Status::NotLoaded) {
            if (item.tries + 1 < kMaxNotLoadedTries) requeue(item, item.tries + 1, 100ms);
            else ++s.skipped;
            continue;
        }
        if (c.status != Status::Ok) {
            ++s.skipped;
            continue;
        }
        const auto hash = static_cast<std::int32_t>(c.chunk->blockBiomeHashCode());
        if (!skipHashes && level->getChunkHash(dh::ChunkPos{item.key.x, item.key.z}) == hash) {
            ++s.unchanged;
            continue;
        }
        auto                                     center = std::make_shared<BedrockChunk>(*c.chunk);
        std::vector<std::shared_ptr<dh::IChunk>> neighbours;
        neighbours.reserve(8);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                // past the budget: only use neighbours that are already snapshotted (border light is a minor loss)
                if (SteadyClock::now() > deadline && !cache.contains({item.key.x + dx, item.key.z + dz})) continue;
                Snap& n = snap(item.key.x + dx, item.key.z + dz);
                if (n.status == Status::Ok) neighbours.push_back(std::make_shared<BedrockChunk>(*n.chunk));
            }
        if (s.world->applyChunkUpdate(*handle, std::move(center), std::move(neighbours))) ++s.ingested;
        else ++s.skipped;
        // Run 13 compare mode: regenerate this real chunk through the distant generator and log the difference.
        if (s.singleplayer && BdsWorldGenerator::compareModeEnabled() && level->isServerLevel()
            && level->isClientLevel())
            if (auto* gen = static_cast<dh::DhClientServerLevel*>(level)->serverside().chunkGenerator())
                static_cast<BdsWorldGenerator*>(gen)->compareWithReal(std::make_shared<BedrockChunk>(*c.chunk));
    }
}

} // namespace dh::bedrock
