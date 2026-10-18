#include "client/render/MaterialInjector.h"

#include "bedrock_common/FileLogger.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/resources/ResourcePackManager.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // std::getenv

namespace dh::render {

namespace {

constexpr char const* kIndexSuffix    = "renderer/materials/materials.index.json";
constexpr char const* kMaterialSuffix = "renderer/materials/DH_Terrain.material.bin";
constexpr char const* kEntry          = "{\"name\":\"DH_Terrain\",\"path\":\"DH_Terrain\"}";

std::atomic<bool> gInstalled{false};
std::atomic<bool> gActive{false};
std::string       gMaterialBytes; // read once at install(), immutable afterwards
std::atomic<int>  gTraceBudget{40};
std::atomic<int>  gProbeBudget{4};

bool endsWith(std::string const& s, char const* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

std::string pathOf(ResourceLocation const& loc) { return loc.mPath.get().value; }

/// Inserts `entry` before the last ']' of the JSON text. Returns false if no array end is found or already present.
bool splice(std::string& json, std::string const& entry) {
    if (json.find("\"DH_Terrain\"") != std::string::npos) return false;
    auto close = json.rfind(']');
    if (close == std::string::npos) return false;
    auto prev  = json.find_last_not_of(" \t\r\n", close - 1);
    bool empty = prev == std::string::npos || json[prev] == '[';
    json.insert(close, (empty ? "" : ",") + entry);
    return true;
}

void trace(char const* tag, std::string const& p) {
    if (p.find("renderer/materials") == std::string::npos) return;
    if (gTraceBudget.fetch_sub(1) > 0) DH_LOG_I("materials: %s %s", tag, p.c_str());
}

LL_TYPE_INSTANCE_HOOK(
    MaterialLoadHook,
    ll::memory::HookPriority::Normal,
    ResourcePackManager,
    static_cast<bool (ResourcePackManager::*)(ResourceLocation const&, std::string&) const>(
        &ResourcePackManager::$load
    ),
    bool,
    ::ResourceLocation const& loc,
    ::std::string&            stream
) {
    if (!gActive.load()) return origin(loc, stream);
    std::string p = pathOf(loc);
    trace("load", p);
    if (endsWith(p, kMaterialSuffix)) {
        stream = gMaterialBytes;
        DH_LOG_I("materials: DH_Terrain served %zu bytes (%s)", stream.size(), p.c_str());
        return true;
    }
    bool ok = origin(loc, stream);
    if (ok && endsWith(p, kIndexSuffix)) {
        size_t before = stream.size();
        if (splice(stream, kEntry))
            DH_LOG_I("materials: materials.index.json spliced (%zu -> %zu bytes)", before, stream.size());
        else DH_LOG_W("materials: materials.index.json splice skipped (%zu bytes, ok=%d)", before, (int)ok);
    } else if (endsWith(p, kIndexSuffix)) {
        DH_LOG_W("materials: materials.index.json origin load failed");
    }
    return ok;
}

// Diagnostic only: the extension-list overload (if the material loader goes through it we need to know).
LL_TYPE_INSTANCE_HOOK(
    MaterialLoadExtHook,
    ll::memory::HookPriority::Normal,
    ResourcePackManager,
    static_cast<bool (ResourcePackManager::*)(ResourceLocation const&, std::string&, gsl::span<std::string const>)
                    const>(&ResourcePackManager::$load),
    bool,
    ::ResourceLocation const&        loc,
    ::std::string&                   stream,
    ::gsl::span<::std::string const> exts
) {
    if (gActive.load()) trace("load(ext)", pathOf(loc));
    return origin(loc, stream, exts);
}

LL_TYPE_INSTANCE_HOOK(
    LoadListProbeHook,
    ll::memory::HookPriority::Normal,
    mce::RenderMaterialGroup,
    &mce::RenderMaterialGroup::loadList,
    bool,
    ::Bedrock::NonOwnerPointer<::mce::ShaderGroup>       shaderGroup,
    ::ResourceLocation const&                            listPath,
    ::ResourcePackManager&                               packs,
    ::Bedrock::NotNullNonOwnerPtr<::ResourceLoadManager> rlm,
    bool                                                 cleanReload
) {
    bool r = origin(std::move(shaderGroup), listPath, packs, std::move(rlm), cleanReload);
    if (gActive.load() && gProbeBudget.fetch_sub(1) > 0) {
        bool resolved = false;
        try {
            auto info = getMaterialInfo(HashedString{"DH_Terrain"});
            resolved  = info && info->mPtr.get() != nullptr;
        } catch (...) {
            DH_LOG_E("materials: probe exception swallowed");
        }
        DH_LOG_I(
            "materials: loadList(%s, clean=%d) -> %d; DH_Terrain resolved: %s",
            pathOf(listPath).c_str(),
            (int)cleanReload,
            (int)r,
            resolved ? "yes" : "no"
        );
    }
    return r;
}

} // namespace

void install() {
    if (gInstalled.exchange(true)) return;
    auto mod = ll::mod::NativeMod::current();
    if (!mod) return;
    auto file = mod->getModDir() / "materials" / "DH_Terrain.material.bin";
    {
        std::ifstream     in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        gMaterialBytes = ss.str();
    }
    bool exists = !gMaterialBytes.empty();
    bool on     = exists;
    if (char const* e = std::getenv("DH_MATERIAL_INJECT")) on = e[0] == '1' && exists;
    if (!on) {
        DH_LOG_I(
            "materials: injector off (file %s: %s)",
            file.string().c_str(),
            exists ? "present, disabled by env" : "missing"
        );
        return;
    }
    gActive.store(true);
    MaterialLoadHook::hook();
    MaterialLoadExtHook::hook();
    LoadListProbeHook::hook();
    DH_LOG_I("materials: injector on, DH_Terrain.material.bin %zu bytes", gMaterialBytes.size());
}

void uninstall() {
    if (!gInstalled.exchange(false)) return;
    if (gActive.exchange(false)) {
        LoadListProbeHook::unhook();
        MaterialLoadExtHook::unhook();
        MaterialLoadHook::unhook();
    }
}

} // namespace dh::render
