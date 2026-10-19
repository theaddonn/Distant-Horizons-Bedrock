#include "client/render/MaterialInjector.h"

#include "bedrock_common/FileLogger.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Symbol.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/deps/core/file/FileOpenMode.h"
#include "mc/deps/core/file/FileSystem.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/file/StandardFileSystem.h"
#include "mc/deps/core/file/file_system/FileSystemImpl.h"
#include "mc/deps/core/platform/Result.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/core/resource/LoadedResourceData.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/resource/ResourceLocationPair.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/resources/AppResourceLoader.h"
#include "mc/resources/ResourcePackManager.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
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

// Diagnostic loader trace: log once per distinct (tag, path) whose path contains "material" (case-insensitive).
std::atomic<int>      gLoaderBudget{200};
std::mutex            gLoaderMutex;
std::set<std::string> gLoaderSeen;

bool traceLoader(char const* tag, std::string_view path) {
    if (path.empty()) return false;
    auto it = std::search(path.begin(), path.end(), "material", "material" + 8, [](char a, char b) {
        return std::tolower((unsigned char)a) == b;
    });
    if (it == path.end()) return false;
    std::string key = std::string(tag) + '|' + std::string(path);
    {
        std::lock_guard lk(gLoaderMutex);
        if (!gLoaderSeen.insert(key).second) return false;
    }
    if (gLoaderBudget.fetch_sub(1) <= 0) return false;
    DH_LOG_I("materials: trace[%s] %.*s", tag, (int)path.size(), path.data());
    return true;
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
            // compiled index entries are looked up in switchable() (R0, 2026-10-19); `this` may be either group
            auto info = getMaterialInfo(HashedString{"DH_Terrain"});
            resolved  = info && info->mPtr.get() != nullptr;
            if (!resolved) {
                auto sw  = mce::RenderMaterialGroup::switchable().getMaterialInfo(HashedString{"DH_Terrain"});
                resolved = sw && sw->mPtr.get() != nullptr;
            }
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

// ---- diagnostic-only trace hooks (log, always call origin, never alter data) ----

constexpr char const* kVtArl = "??_7AppResourceLoader@@6B@";
constexpr char const* kVtSfs = "??_7StandardFileSystem@Core@@6B@";

// The `$fn` thunks of these classes are not exported by the import lib; hook the vtable slot instead.
template <class T, class MemFn>
uintptr_t vslot(char const* vftableSym, MemFn fn) {
    auto idx = ll::memory::getVtableIndex(fn);
    auto vt  = static_cast<void**>(ll::memory::SymbolView{vftableSym}.resolve(true));
    if (!idx || !vt) return 0;
    return reinterpret_cast<uintptr_t>(vt[*idx]);
}

uintptr_t slot_TraceArlLoad1() {
    return vslot<AppResourceLoader>(
        kVtArl,
        static_cast<bool (AppResourceLoader::*)(ResourceLocation const&, std::string&) const>(&AppResourceLoader::load)
    );
}

LL_TYPE_INSTANCE_HOOK(
    TraceArlLoad1,
    ll::memory::HookPriority::Normal,
    AppResourceLoader,
    slot_TraceArlLoad1(),
    bool,
    ::ResourceLocation const& loc,
    ::std::string&            stream
) {
    bool r = origin(loc, stream);
    if (gActive.load() && traceLoader("AppResourceLoader::$load", pathOf(loc)))
        DH_LOG_I("materials: trace   -> ok=%d size=%zu", (int)r, stream.size());
    return r;
}

uintptr_t slot_TraceArlLoad2() {
    return vslot<AppResourceLoader>(
        kVtArl,
        static_cast<bool (AppResourceLoader::*)(ResourceLocation const&, std::string&, gsl::span<std::string const>)
                        const>(&AppResourceLoader::load)
    );
}

LL_TYPE_INSTANCE_HOOK(
    TraceArlLoad2,
    ll::memory::HookPriority::Normal,
    AppResourceLoader,
    slot_TraceArlLoad2(),
    bool,
    ::ResourceLocation const&        loc,
    ::std::string&                   stream,
    ::gsl::span<::std::string const> exts
) {
    bool r = origin(loc, stream, exts);
    if (gActive.load() && traceLoader("AppResourceLoader::$load(ext)", pathOf(loc)))
        DH_LOG_I("materials: trace   -> ok=%d size=%zu", (int)r, stream.size());
    return r;
}

uintptr_t slot_TraceArlLoad3() {
    return vslot<AppResourceLoader>(
        kVtArl,
        static_cast<bool (AppResourceLoader::*)(ResourceLocationPair const&, std::string&, gsl::span<std::string const>)
                        const>(&AppResourceLoader::load)
    );
}

LL_TYPE_INSTANCE_HOOK(
    TraceArlLoad3,
    ll::memory::HookPriority::Normal,
    AppResourceLoader,
    slot_TraceArlLoad3(),
    bool,
    ::ResourceLocationPair const&    loc,
    ::std::string&                   stream,
    ::gsl::span<::std::string const> exts
) {
    bool r = origin(loc, stream, exts);
    if (gActive.load() && traceLoader("AppResourceLoader::$load(pair)", pathOf(loc.mResourceLocation.get())))
        DH_LOG_I("materials: trace   -> ok=%d size=%zu", (int)r, stream.size());
    return r;
}

uintptr_t slot_TraceArlAllVersions() { return vslot<AppResourceLoader>(kVtArl, &AppResourceLoader::loadAllVersionsOf); }

LL_TYPE_INSTANCE_HOOK(
    TraceArlAllVersions,
    ll::memory::HookPriority::Normal,
    AppResourceLoader,
    slot_TraceArlAllVersions(),
    std::vector<LoadedResourceData>,
    ::ResourceLocation const& loc
) {
    auto r = origin(loc);
    if (gActive.load()) traceLoader("AppResourceLoader::$loadAllVersionsOf", pathOf(loc));
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    TraceRpmAllVersions,
    ll::memory::HookPriority::Normal,
    ResourcePackManager,
    &ResourcePackManager::$loadAllVersionsOf,
    std::vector<LoadedResourceData>,
    ::ResourceLocation const& loc
) {
    auto r = origin(loc);
    if (gActive.load()) traceLoader("ResourcePackManager::$loadAllVersionsOf", pathOf(loc));
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    TraceRpmLoadNv,
    ll::memory::HookPriority::Normal,
    ResourcePackManager,
    &ResourcePackManager::load,
    bool,
    ::ResourceLocation const&    loc,
    ::std::string&               stream,
    ::std::optional<::mce::UUID> packId
) {
    bool r = origin(loc, stream, packId);
    if (gActive.load() && traceLoader("ResourcePackManager::load", pathOf(loc)))
        DH_LOG_I("materials: trace   -> ok=%d size=%zu", (int)r, stream.size());
    return r;
}

uintptr_t slot_TraceSfsReadFile() {
    return vslot<Core::StandardFileSystem>(kVtSfs, &Core::StandardFileSystem::readFile);
}

LL_TYPE_INSTANCE_HOOK(
    TraceSfsReadFile,
    ll::memory::HookPriority::Normal,
    Core::StandardFileSystem,
    slot_TraceSfsReadFile(),
    ::Bedrock::Result<std::string>,
    ::Core::PathView path
) {
    auto r = origin(path);
    if (gActive.load() && traceLoader("StandardFileSystem::$readFile", path.getUtf8StringView()))
        DH_LOG_I("materials: trace   -> has_value=%d size=%zu", (int)r.has_value(), r.has_value() ? r->size() : 0);
    return r;
}

uintptr_t slot_TraceSfsReadFileData() {
    return vslot<Core::StandardFileSystem>(kVtSfs, &Core::StandardFileSystem::readFileData);
}

LL_TYPE_INSTANCE_HOOK(
    TraceSfsReadFileData,
    ll::memory::HookPriority::Normal,
    Core::StandardFileSystem,
    slot_TraceSfsReadFileData(),
    ::Core::Result,
    ::Core::PathView              path,
    ::std::vector<unsigned char>& data
) {
    auto r = origin(path, data);
    if (gActive.load() && traceLoader("StandardFileSystem::$readFileData", path.getUtf8StringView()))
        DH_LOG_I("materials: trace   -> size=%zu", data.size());
    return r;
}

uintptr_t slot_TraceSfsOpenFile() {
    return vslot<Core::StandardFileSystem>(kVtSfs, &Core::StandardFileSystem::openFile);
}

LL_TYPE_INSTANCE_HOOK(
    TraceSfsOpenFile,
    ll::memory::HookPriority::Normal,
    Core::StandardFileSystem,
    slot_TraceSfsOpenFile(),
    ::Core::Result,
    ::Core::PathView            path,
    ::Core::File&               fileOut,
    ::Core::FileOpenMode const& mode,
    ::Core::FileBufferingMode   buf
) {
    if (gActive.load()) traceLoader("StandardFileSystem::$openFile", path.getUtf8StringView());
    return origin(path, fileOut, mode, buf);
}

uintptr_t slot_TraceSfsReadFileDataExt() {
    return vslot<Core::StandardFileSystem>(kVtSfs, &Core::StandardFileSystem::readFileDataExt);
}

LL_TYPE_INSTANCE_HOOK(
    TraceSfsReadFileDataExt,
    ll::memory::HookPriority::Normal,
    Core::StandardFileSystem,
    slot_TraceSfsReadFileDataExt(),
    ::Core::Result,
    ::Core::PathView              path,
    uint64                        pos,
    uint64                        bufSize,
    ::std::vector<unsigned char>& buffer,
    uint64&                       bytesRead
) {
    auto r = origin(path, pos, bufSize, buffer, bytesRead);
    if (gActive.load() && traceLoader("StandardFileSystem::$readFileDataExt", path.getUtf8StringView()))
        DH_LOG_I(
            "materials: trace   -> pos=%llu bytesRead=%llu",
            (unsigned long long)pos,
            (unsigned long long)bytesRead
        );
    return r;
}

//=====================================================================================================================
// Win32 file redirection. The trace run (2026-10-19) showed the compiled-material loader never goes through
// ResourcePackManager / AppResourceLoader / Core::StandardFileSystem: only the legacy `.material` lists do. So the
// index and the .material.bin files are opened with plain Win32 calls. CreateFileW/CreateFile2 (+ the attribute
// probes a loader does before opening) are hooked: a request for `<game>\...\renderer\materials\materials.index.json`
// is answered with a patched copy in the mod dir (original + our entries), and `DH_*.material.bin` with the mod's
// file. Nothing in the game directory is written.
//=====================================================================================================================

std::filesystem::path gModMaterialsDir; // <modDir>/materials
std::filesystem::path gPatchedIndex;    // <modDir>/materials/materials.index.json (generated)
std::atomic<bool>     gPatchedIndexReady{false};
std::mutex            gPatchMutex;
thread_local bool     tlInFileHook = false; // our own std::ifstream/ofstream must not be redirected

std::string narrow(wchar_t const* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string lowerSlashes(std::string s) {
    for (auto& c : s) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Builds <modDir>/materials/materials.index.json from the game's original (read directly, not via the hook).
bool preparePatchedIndex(std::string const& originalPath) {
    std::lock_guard lock(gPatchMutex);
    if (gPatchedIndexReady.load()) return true;
    tlInFileHook = true;
    std::string json;
    {
        std::ifstream in(std::filesystem::path(originalPath), std::ios::binary);
        if (!in) {
            tlInFileHook = false;
            DH_LOG_W("materials: cannot read the game's index at %s", originalPath.c_str());
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        json = ss.str();
    }
    size_t before = json.size();
    bool   added  = splice(json, kEntry);
    std::filesystem::create_directories(gModMaterialsDir);
    {
        std::ofstream out(gPatchedIndex, std::ios::binary | std::ios::trunc);
        out << json;
    }
    tlInFileHook = false;
    gPatchedIndexReady.store(true);
    DH_LOG_I(
        "materials: patched index written to %s (%zu -> %zu bytes, DH_Terrain %s)",
        gPatchedIndex.string().c_str(),
        before,
        json.size(),
        added ? "added" : "already present"
    );
    return true;
}

/// The mod-dir path the game should get instead of `path` (empty = no redirection).
std::wstring redirectFor(std::string const& path) {
    const std::string p = lowerSlashes(path);
    if (p.find("renderer\\materials\\") == std::string::npos) return {};
    if (endsWith(p, "\\materials.index.json")) {
        if (!preparePatchedIndex(path)) return {};
        return gPatchedIndex.wstring();
    }
    if (endsWith(p, "\\dh_terrain.material.bin")) {
        return (gModMaterialsDir / "DH_Terrain.material.bin").wstring();
    }
    return {};
}

struct FileHookGuard {
    FileHookGuard() { tlInFileHook = true; }
    ~FileHookGuard() { tlInFileHook = false; }
};

uintptr_t k32(char const* name) {
    HMODULE m = GetModuleHandleW(L"kernel32.dll");
    return m ? reinterpret_cast<uintptr_t>(GetProcAddress(m, name)) : 0;
}

LL_STATIC_HOOK(
    HookCreateFileW,
    ll::memory::HookPriority::Normal,
    k32("CreateFileW"),
    HANDLE,
    LPCWSTR               name,
    DWORD                 access,
    DWORD                 share,
    LPSECURITY_ATTRIBUTES sa,
    DWORD                 disposition,
    DWORD                 flags,
    HANDLE                templ
) {
    if (gActive.load() && name && !tlInFileHook) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            traceLoader("CreateFileW", n);
            std::wstring r = redirectFor(n);
            if (!r.empty()) {
                DH_LOG_I("materials: CreateFileW redirect %s -> %s", n.c_str(), narrow(r.c_str()).c_str());
                return origin(r.c_str(), access, share, sa, disposition, flags, templ);
            }
        }
    }
    return origin(name, access, share, sa, disposition, flags, templ);
}

LL_STATIC_HOOK(
    HookCreateFile2,
    ll::memory::HookPriority::Normal,
    k32("CreateFile2"),
    HANDLE,
    LPCWSTR                           name,
    DWORD                             access,
    DWORD                             share,
    DWORD                             disposition,
    LPCREATEFILE2_EXTENDED_PARAMETERS params
) {
    if (gActive.load() && name && !tlInFileHook) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            traceLoader("CreateFile2", n);
            std::wstring r = redirectFor(n);
            if (!r.empty()) {
                DH_LOG_I("materials: CreateFile2 redirect %s -> %s", n.c_str(), narrow(r.c_str()).c_str());
                return origin(r.c_str(), access, share, disposition, params);
            }
        }
    }
    return origin(name, access, share, disposition, params);
}

LL_STATIC_HOOK(
    HookGetFileAttributesExW,
    ll::memory::HookPriority::Normal,
    k32("GetFileAttributesExW"),
    BOOL,
    LPCWSTR                name,
    GET_FILEEX_INFO_LEVELS level,
    LPVOID                 info
) {
    if (gActive.load() && name && !tlInFileHook) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            traceLoader("GetFileAttributesExW", n);
            std::wstring r = redirectFor(n);
            if (!r.empty()) return origin(r.c_str(), level, info);
        }
    }
    return origin(name, level, info);
}

LL_STATIC_HOOK(
    HookGetFileAttributesW,
    ll::memory::HookPriority::Normal,
    k32("GetFileAttributesW"),
    DWORD,
    LPCWSTR name
) {
    if (gActive.load() && name && !tlInFileHook) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            traceLoader("GetFileAttributesW", n);
            std::wstring r = redirectFor(n);
            if (!r.empty()) return origin(r.c_str());
        }
    }
    return origin(name);
}

} // namespace

#define DH_TRACE_HOOK(H) DH_LOG_I("materials: trace hook " #H " -> %d", (int)H::hook())
// vtable-slot hooks: never hand the hook engine a null target
#define DH_TRACE_VHOOK(H)                                                                                              \
    do {                                                                                                               \
        if (slot_##H()) DH_TRACE_HOOK(H);                                                                              \
        else DH_LOG_W("materials: trace hook " #H " skipped (vtable slot unresolved)");                                \
    } while (0)

void installMaterialInjector() {
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
    gModMaterialsDir = mod->getModDir() / "materials";
    gPatchedIndex    = gModMaterialsDir / "materials.index.json";
    gPatchedIndexReady.store(false);
    DH_LOG_I(
        "materials: file hooks CreateFileW=%d CreateFile2=%d GetFileAttributesExW=%d GetFileAttributesW=%d (0 = ok)",
        HookCreateFileW::hook(),
        HookCreateFile2::hook(),
        HookGetFileAttributesExW::hook(),
        HookGetFileAttributesW::hook()
    );
    MaterialLoadHook::hook();
    MaterialLoadExtHook::hook();
    LoadListProbeHook::hook();
    DH_TRACE_VHOOK(TraceArlLoad1);
    DH_TRACE_VHOOK(TraceArlLoad2);
    DH_TRACE_VHOOK(TraceArlLoad3);
    DH_TRACE_VHOOK(TraceArlAllVersions);
    DH_TRACE_HOOK(TraceRpmAllVersions);
    DH_TRACE_HOOK(TraceRpmLoadNv);
    DH_TRACE_VHOOK(TraceSfsReadFile);
    DH_TRACE_VHOOK(TraceSfsReadFileData);
    DH_TRACE_VHOOK(TraceSfsOpenFile);
    DH_TRACE_VHOOK(TraceSfsReadFileDataExt);
    DH_LOG_I("materials: injector on, DH_Terrain.material.bin %zu bytes", gMaterialBytes.size());
}

void uninstallMaterialInjector() {
    if (!gInstalled.exchange(false)) return;
    if (gActive.exchange(false)) {
        HookGetFileAttributesW::unhook();
        HookGetFileAttributesExW::unhook();
        HookCreateFile2::unhook();
        HookCreateFileW::unhook();
        TraceSfsReadFileDataExt::unhook();
        TraceSfsOpenFile::unhook();
        TraceSfsReadFileData::unhook();
        TraceSfsReadFile::unhook();
        TraceRpmLoadNv::unhook();
        TraceRpmAllVersions::unhook();
        TraceArlAllVersions::unhook();
        TraceArlLoad3::unhook();
        TraceArlLoad2::unhook();
        TraceArlLoad1::unhook();
        LoadListProbeHook::unhook();
        MaterialLoadExtHook::unhook();
        MaterialLoadHook::unhook();
    }
}

} // namespace dh::render
