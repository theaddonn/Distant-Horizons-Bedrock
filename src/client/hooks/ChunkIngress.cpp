#include "client/hooks/ChunkIngress.h"

#include "bedrock_common/BedrockChunk.h"
#include "bedrock_common/BlockSerial.h"
#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/hud/DebugHud.h"

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/platform/BlockTable.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/LevelChunk.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dh::ingress {

namespace {

std::atomic<bool> gEnabled{false};
std::atomic<bool> gInstalled{false};
std::atomic<int>  gChunks{0};

std::mutex                                    gMutex;
std::optional<dh::BlockTable>                 gTable;
std::unique_ptr<dh::bedrock::BlockSerializer> gSerializer;
bool                                          gLoadFailed = false;

std::string slurp(const std::filesystem::path& p) {
    std::ifstream     in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Loads <modDir>/data/{block,biome}_table.json once. Returns the serializer or nullptr.
dh::bedrock::BlockSerializer* serializer() {
    std::lock_guard lock(gMutex);
    if (gSerializer) return gSerializer.get();
    if (gLoadFailed) return nullptr;
    auto mod = ll::mod::NativeMod::current();
    if (!mod) return nullptr;
    auto dir = mod->getModDir() / "data";
    auto r   = dh::BlockTable::load(slurp(dir / "block_table.json"), slurp(dir / "biome_table.json"));
    if (!r.has_value()) {
        gLoadFailed = true;
        DH_LOG_E("ingress: cannot load block tables from %s: %s", dir.string().c_str(), r.error().message.c_str());
        return nullptr;
    }
    gTable.emplace(std::move(r.value()));
    gSerializer = std::make_unique<dh::bedrock::BlockSerializer>(*gTable);
    DH_LOG_I("ingress: block table loaded (%zu blocks, %zu biomes)", gTable->blockCount(), gTable->biomeCount());
    return gSerializer.get();
}

void processChunkImpl(LevelChunk& lc) {
    auto* ser = serializer();
    if (!ser) return;
    if (!dh::bedrock::BedrockChunk::heightSupported(lc)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) DH_LOG_W("ingress: dimension height is not a multiple of 16, chunks skipped");
        return;
    }
    using clock = std::chrono::steady_clock;
    auto t0     = clock::now();

    dh::bedrock::BedrockChunk chunk(lc, *ser);
    dh::LightingOptions       lo;
    dh::lightChunk(chunk, {}, lo);
    auto src = dh::createFromChunk(chunk, dh::BuildOptions{});

    double ms      = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    int    columns = 0;
    size_t ids     = 0;
    if (src) {
        const int ox = dh::chunkOffsetInSection(chunk.chunkX());
        const int oz = dh::chunkOffsetInSection(chunk.chunkZ());
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z)
                if (!src->column(ox + x, oz + z).empty()) ++columns;
        ids = src->mapping.size();
    }
    DH_LOG_I(
        "chunk %d,%d: %d columns, %zu ids, %.2f ms%s",
        chunk.chunkX(),
        chunk.chunkZ(),
        columns,
        ids,
        ms,
        src ? "" : " (build failed)"
    );
    int n = ++gChunks;
    dh::hud::set("ingress.chunks", n);
    dh::hud::set("ingress.lastMs", ms);
}

void processChunk(LevelChunk& lc) {
    try {
        processChunkImpl(lc);
    } catch (...) {
        DH_LOG_E("chunk ingress: exception swallowed");
    }
}

LL_TYPE_INSTANCE_HOOK(
    IngressOnChunkLoadedHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$onChunkLoaded,
    void,
    ::ChunkSource& source,
    ::LevelChunk&  lc
) {
    origin(source, lc);
    if (gEnabled.load()) processChunk(lc);
}

ll::event::ListenerPtr gKeyListener;
ll::event::ListenerPtr gExitListener;

void onKeys(ll::event::AfterUIRenderEvent& e) {
    if (e.screenView().getScreenName() != "hud_screen") return;
    if (dh::input::pressed(VK_F6)) {
        bool on = !gEnabled.load();
        gEnabled.store(on);
        DH_LOG_I("ingress: %s (F6)", on ? "on" : "off");
        dh::hud::set("ingress.on", on ? "on" : "off");
    }
}

} // namespace

void install() {
    if (gInstalled.exchange(true)) return;
    IngressOnChunkLoadedHook::hook();
    auto& bus     = ll::event::EventBus::getInstance();
    gKeyListener  = bus.emplaceListener<ll::event::AfterUIRenderEvent>(onKeys);
    gExitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
        std::lock_guard lock(gMutex);
        if (gSerializer) gSerializer->clear(); // Block*/Biome* die with the level's registries
    });
    dh::hud::set("ingress.on", "off");
    dh::hud::set("ingress.chunks", 0);
    DH_LOG_I("ingress installed (off; F6 toggles chunk -> LOD smoke test)");
}

void uninstall() {
    if (!gInstalled.exchange(false)) return;
    gEnabled.store(false);
    auto& bus = ll::event::EventBus::getInstance();
    if (gKeyListener) bus.removeListener(gKeyListener);
    if (gExitListener) bus.removeListener(gExitListener);
    gKeyListener.reset();
    gExitListener.reset();
    IngressOnChunkLoadedHook::unhook();
}

} // namespace dh::ingress
