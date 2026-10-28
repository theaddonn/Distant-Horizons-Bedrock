#include "client/render/MaterialInjector.h"

#include "bedrock_common/FileLogger.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Signature.h"
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
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/RenderMaterial.h"
#include "mc/resources/AppResourceLoader.h"
#include "mc/resources/ResourcePackManager.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // std::getenv

namespace dh::render {

namespace {

constexpr char const* kIndexSuffix    = "renderer/materials/materials.index.json";
constexpr char const* kMaterialSuffix = "renderer/materials/DH_Terrain.material.bin";
// Four index entries share the one compiled material file: DH_TerrainTransparent (blending, no depth write),
// DH_TerrainLate (opaque output, depth write, but flagged Blending so the dragon frame graph bins it into the
// Transparent pass after the stars / sun / moon, Run 13) and DH_TerrainWater (blending WITH depth write, the LOD
// translucent layer in the overdraw sky order, Run 14) are only extra mce render-state holders; the dragon request
// rename maps all of them to DH_Terrain.
constexpr char const* kEntry = "{\"name\":\"DH_Terrain\",\"path\":\"DH_Terrain\"},"
                               "{\"name\":\"DH_TerrainTransparent\",\"path\":\"DH_Terrain\"},"
                               "{\"name\":\"DH_TerrainLate\",\"path\":\"DH_Terrain\"},"
                               "{\"name\":\"DH_TerrainWater\",\"path\":\"DH_Terrain\"}";

std::atomic<bool> gInstalled{false};
std::atomic<bool> gActive{false};
std::atomic<bool> gBinderInstalled{false};
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

// File-redirect logging is bounded: the material files are re-opened on every resource reload.
std::atomic<int> gRedirectLogBudget{16};
bool             redirectLog() { return gRedirectLogBudget.fetch_sub(1) > 0; }

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
/// Per-thread flag in a dynamic TLS slot. NOT `thread_local`: this DLL is loaded after the process started, and a
/// thread created before that has no static-TLS block for it (Wine); the 2026-10-19 crash was the material-cache hook
/// reading `thread_local bool` on such a thread (gs:[0x58][tls_index] == NULL). TlsGetValue works on every thread.
/// GetLastError is preserved so the file hooks keep the Win32 error of the call they wrap.
struct TlsFlag {
    DWORD idx = TlsAlloc();
    bool  get() const {
        if (idx == TLS_OUT_OF_INDEXES) return false;
        const DWORD e = GetLastError();
        const bool  v = TlsGetValue(idx) != nullptr;
        SetLastError(e);
        return v;
    }
    void set(bool v) const {
        if (idx == TLS_OUT_OF_INDEXES) return;
        const DWORD e = GetLastError();
        TlsSetValue(idx, v ? reinterpret_cast<void*>(1) : nullptr);
        SetLastError(e);
    }
};
TlsFlag gInFileHook; // our own std::ifstream/ofstream must not be redirected

//---------------------------------------------------------------------------------------------------------------------
// Freeze watch (2026-10-19 freeze at pipeline-on, root cause unknown). Each engine-facing hook marks "thread T entered
// site S at tick X"; a low-rate watchdog thread reports any site that has been in flight for > 3 s ONCE, with the stuck
// thread's RIP / RSP and the module+RVA of every code pointer in the top of its stack, so the next occurrence names
// the blocking call (symbolise with ~/.cache/dh-re). Cost on the hot paths: two relaxed atomic stores per hook call;
// nothing is logged unless a hook is stuck (3 s; 10 s for enqueueAsync/getMaterial). Report goes to logs/dh-freeze.log via WriteFile (not the logger); the suspended thread
// might hold the logger mutex).
//---------------------------------------------------------------------------------------------------------------------
enum WatchSite { kWsResolve, kWsEnqueue, kWsGetMaterial, kWsCreateFile, kWsPatchIndex, kWsCount };
constexpr char const* kWatchNames[kWsCount] = {
    "resolveDragonMaterial", "enqueueAsync", "getMaterial", "CreateFileW redirect", "patched index"
};
// enqueueAsync / getMaterial legitimately run long during asset compiles: higher threshold there.
constexpr ULONGLONG kWatchLimitMs[kWsCount] = {3000, 10000, 10000, 3000, 3000};

/// One slot per site: tid in the high 32 bits, (entry tick - gWatchEpoch) in the low 32 bits, stored and loaded as a
/// unit (0 = idle) so the watchdog never pairs a stale tick with another thread's id. A second thread entering a site
/// that is already occupied is simply not tracked (CAS 0 -> packed fails; its destructor then does nothing).
struct WatchSlot {
    std::atomic<std::uint64_t> packed{0};
    std::uint64_t              reported = 0; // watchdog-only
};
WatchSlot         gWatch[kWsCount];
ULONGLONG         gWatchEpoch = GetTickCount64();
std::atomic<bool> gWatchStop{false};
std::atomic<bool> gWatchDone{true};
std::thread       gWatchThread;
std::wstring      gFreezeLogPath; // <modDir>/logs/dh-freeze.log, set before the watchdog starts

struct WatchGuard {
    WatchSite     site;
    std::uint64_t mine = 0;
    explicit WatchGuard(WatchSite s) : site(s) {
        const std::uint64_t p =
            (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32) | static_cast<std::uint32_t>(GetTickCount64() - gWatchEpoch);
        std::uint64_t expect = 0;
        if (gWatch[s].packed.compare_exchange_strong(expect, p, std::memory_order_relaxed)) mine = p;
    }
    ~WatchGuard() {
        if (mine) {
            std::uint64_t expect = mine;
            gWatch[site].packed.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
        }
    }
};

std::string modRva(void* p) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(p),
            &mod
        )
        || !mod)
        return {};
    wchar_t     w[MAX_PATH];
    std::string name = "?";
    if (GetModuleFileNameW(mod, w, MAX_PATH)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            name.assign(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, name.data(), n, nullptr, nullptr);
        }
        auto sl = name.find_last_of("\\/");
        if (sl != std::string::npos) name = name.substr(sl + 1);
    }
    char b[64];
    snprintf(
        b,
        sizeof(b),
        "+0x%llx",
        (unsigned long long)(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(mod))
    );
    return name + b;
}

/// While the target is suspended ONLY the context and the raw stack words are copied into fixed stack buffers: no
/// allocation, no module lookup (the target may hold the loader or heap lock). Everything else happens after
/// ResumeThread. The report goes straight to a side file with Win32 WriteFile, never through the logger (the target
/// may hold the logger lock).
void reportStuck(int site, DWORD tid, ULONGLONG ms) {
    uintptr_t rip = 0, buf[96]{};
    SIZE_T    rd = 0;
    bool      got = false;
    HANDLE    h  = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (h) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(h, &ctx)) {
                got = true;
                rip = ctx.Rip;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), buf, sizeof(buf), &rd))
                    rd = 0;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    std::string out;
    if (got) {
        out = "rip=" + modRva(reinterpret_cast<void*>(rip));
        int shown = 0;
        for (SIZE_T i = 0; i < rd / sizeof(uintptr_t) && shown < 14; ++i) {
            std::string m = modRva(reinterpret_cast<void*>(buf[i]));
            if (m.empty()) continue;
            out += (shown++ ? " < " : " stack: ") + m;
        }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    int  n = snprintf(
        line,
        sizeof(line),
        "[%02d:%02d:%02d.%03d] materials: freeze watch: thread %lx stuck in %s for %llu ms %s\r\n",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        (unsigned long)tid,
        kWatchNames[site],
        (unsigned long long)ms,
        out.empty() ? "(context unavailable)" : out.c_str()
    );
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;
    if (gFreezeLogPath.empty()) return;
    HANDLE f = CreateFileW(
        gFreezeLogPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (f != INVALID_HANDLE_VALUE) {
        // bounded: a stuck-forever hook is reported once per episode, but never let the file grow past 256 KB
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(f, &sz) && sz.QuadPart < 256 * 1024) {
            DWORD wr = 0;
            WriteFile(f, line, static_cast<DWORD>(n), &wr, nullptr);
        }
        CloseHandle(f);
    }
}

void watchdogMain() {
    while (!gWatchStop.load()) {
        for (int i = 0; i < 20 && !gWatchStop.load(); ++i) Sleep(50);
        const ULONGLONG now32 = static_cast<std::uint32_t>(GetTickCount64() - gWatchEpoch);
        for (int s = 0; s < kWsCount; ++s) {
            const std::uint64_t p = gWatch[s].packed.load(std::memory_order_relaxed);
            if (!p || gWatch[s].reported == p) continue;
            const ULONGLONG age = static_cast<std::uint32_t>(now32 - (p & 0xffffffffu));
            if (age < kWatchLimitMs[s]) continue;
            gWatch[s].reported = p;
            reportStuck(s, static_cast<DWORD>(p >> 32), age);
        }
    }
    gWatchDone.store(true);
}

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
    WatchGuard wg(kWsPatchIndex);
    gInFileHook.set(true);
    std::string json;
    {
        std::ifstream in(std::filesystem::path(originalPath), std::ios::binary);
        if (!in) {
            gInFileHook.set(false);
            DH_LOG_W("materials: cannot read the game's index at %s", originalPath.c_str());
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        json = ss.str();
    }
    size_t before = json.size();
    bool   added  = splice(json, kEntry);
    std::error_code ec; // non-throwing: an exception here would leave gInFileHook set on this thread
    std::filesystem::create_directories(gModMaterialsDir, ec);
    if (ec) DH_LOG_W("materials: create_directories(%s) failed: %s", gModMaterialsDir.string().c_str(), ec.message().c_str());
    {
        std::ofstream out(gPatchedIndex, std::ios::binary | std::ios::trunc);
        out << json;
    }
    gInFileHook.set(false);
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
    FileHookGuard() { gInFileHook.set(true); }
    ~FileHookGuard() { gInFileHook.set(false); }
};

/// Logs the return-address chain of the current thread as module+RVA, so the offline disassembly can start at the
/// exact functions that open a compiled material (who calls CompiledMaterialManager::getMaterial, and from where).
std::atomic<int> gBacktraceBudget{6};
void             logBacktrace(char const* why) {
    if (gBacktraceBudget.fetch_sub(1) <= 0) return;
    void*       frames[32];
    USHORT      n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    std::string line;
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod      = nullptr;
        char    name[64] = "?";
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(frames[i]),
                &mod
            )
            && mod) {
            wchar_t w[MAX_PATH];
            if (GetModuleFileNameW(mod, w, MAX_PATH)) {
                std::string s = narrow(w);
                auto        p = s.find_last_of("\\/");
                s             = p == std::string::npos ? s : s.substr(p + 1);
                snprintf(name, sizeof(name), "%s", s.c_str());
            }
        }
        char buf[128];
        snprintf(
            buf,
            sizeof(buf),
            "%s%s+0x%llx",
            i ? " < " : "",
            name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod))
        );
        line += buf;
    }
    DH_LOG_I("materials: backtrace (%s): %s", why, line.c_str());
}

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
    if (gActive.load() && name && !gInFileHook.get()) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            WatchGuard    wg(kWsCreateFile);
            traceLoader("CreateFileW", n);
            if (endsWith(lowerSlashes(n), ".material.bin") && gRedirectLogBudget.load() > 0) logBacktrace(n.c_str());
            std::wstring r = redirectFor(n);
            if (!r.empty()) {
                const bool logIt = redirectLog();
                if (logIt) DH_LOG_I("materials: CreateFileW redirect %s -> %s", n.c_str(), narrow(r.c_str()).c_str());
                HANDLE h = origin(r.c_str(), access, share, sa, disposition, flags, templ);
                if (logIt) {
                    const DWORD e = GetLastError(); // breadcrumb (2026-10-19 freeze right after this line)
                    DH_LOG_I("materials: CreateFileW redirect -> handle %s", h == INVALID_HANDLE_VALUE ? "INVALID" : "ok");
                    SetLastError(e);
                }
                return h;
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
    if (gActive.load() && name && !gInFileHook.get()) {
        std::string n = narrow(name);
        if (lowerSlashes(n).find("material") != std::string::npos) {
            FileHookGuard g;
            traceLoader("CreateFile2", n);
            std::wstring r = redirectFor(n);
            if (!r.empty()) {
                if (redirectLog()) DH_LOG_I("materials: CreateFile2 redirect %s -> %s", n.c_str(), narrow(r.c_str()).c_str());
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
    if (gActive.load() && name && !gInFileHook.get()) {
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
    if (gActive.load() && name && !gInFileHook.get()) {
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

//=====================================================================================================================
// dragon::materials::CompiledMaterialManager::getMaterial(std::string name, SimplePassphrase, bool forceReload)
//   -> std::shared_ptr<CompiledMaterialDefinition>
// The engine's by-name entry point for compiled materials (what FrederoxDev/Amethyst hooks). Not in the SDK headers;
// the mangled name comes from the RTTI of a lambda inside it in Minecraft.Windows.exe, resolved at runtime through
// LeviLamina's client symbol data. Member-function ABI: `this` in rcx, the shared_ptr return slot in rdx, the
// by-value std::string / SimplePassphrase as pointers to caller temporaries - so the detour is declared as an
// instance hook on a dummy class with reference/pointer parameters that are passed through untouched.
//=====================================================================================================================

//---------------------------------------------------------------------------------------------------------------------
// Signature-resolved engine functions (Minecraft.Windows.exe 1.26.51; see the internal stage-b-offline spike notes). Raw-ABI
// detours: sret pointers are explicit parameters and the functions return them in rax.
//---------------------------------------------------------------------------------------------------------------------

struct SigDef {
    char const* name;
    char const* sig;
};
constexpr SigDef kSigResolve = {
    "resolveDragonMaterial",
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 04 00 00 48 8D AC 24 80 00 00 00 44 0F 29 95 20 04 00 00"
};
constexpr SigDef kSigEnqueue = {
    "enqueueAsync",
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 C0 00 00 00 FE FF FF FF "
    "49 89 D5"
};
constexpr SigDef kSigCache = {
    "cacheGetOrCreate",
    "55 41 57 41 56 41 54 56 57 53 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 4D 89 C6 48 89 D6 48 89 CF 49 8B "
    "00 41 0F B6 48 08"
};
constexpr SigDef kSigGetMaterial = {
    "getMaterial",
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 A0 00 00 00 FE FF FF FF "
    "4C 89 CB 4C 89 C6 49 89 D6 49 89 CF"
};

uintptr_t gAddrResolve = 0, gAddrEnqueue = 0, gAddrCache = 0, gAddrGetMaterial = 0;

uintptr_t resolveSig(SigDef const& d, bool requireUnique) {
    auto*     first = ll::memory::SignatureView{ll::memory::Signature::parse(d.sig)}.resolve(true);
    uintptr_t addr  = reinterpret_cast<uintptr_t>(first);
    auto      base  = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!addr) {
        DH_LOG_I("materials: sig %s -> none", d.name);
        return 0;
    }
    if (requireUnique) {
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto  end = base + nt->OptionalHeader.SizeOfImage;
        if (addr + 1 < end) {
            auto* second = ll::memory::SignatureView{ll::memory::Signature::parse(d.sig)}.resolve(
                std::span<std::byte>{reinterpret_cast<std::byte*>(addr + 1), static_cast<size_t>(end - addr - 1)},
                true
            );
            if (second) {
                DH_LOG_W(
                    "materials: sig %s -> ambiguous (0x%llx and 0x%llx), not used",
                    d.name,
                    (unsigned long long)(addr - base),
                    (unsigned long long)(reinterpret_cast<uintptr_t>(second) - base)
                );
                return 0;
            }
        }
    }
    DH_LOG_I("materials: sig %s -> 0x%llx", d.name, (unsigned long long)(addr - base));
    return addr;
}

mce::RenderMaterialInfo* gDhInfo       = nullptr; // the mce info of "DH_Terrain" (cloned from the vanilla fallback)
mce::RenderMaterialInfo* gDhTransInfo  = nullptr; // "DH_TerrainTransparent" (same compiled material, blended state)
mce::RenderMaterialInfo* gDhLateInfo   = nullptr; // "DH_TerrainLate" (opaque output, Blending flag: late pass bin)
mce::RenderMaterialInfo* gDhWaterInfo  = nullptr; // "DH_TerrainWater" (blended like Transparent, but depth write)
TlsFlag                  gDhResolving; // dynamic TLS, see TlsFlag
std::atomic<std::uint64_t> gDhResolves[4]{}; // dragon binding resolves per clone (render: perf line)

int kindOf(void const* info) {
    return info == gDhInfo ? 0 : info == gDhTransInfo ? 1 : info == gDhLateInfo ? 2 : 3;
}
bool                     gKeyRewrite   = false;

struct ResolvingGuard {
    bool prev;
    ResolvingGuard() : prev(gDhResolving.get()) { gDhResolving.set(true); }
    ~ResolvingGuard() { gDhResolving.set(prev); }
};

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

LL_STATIC_HOOK(
    HookResolveDragon,
    ll::memory::HookPriority::Normal,
    gAddrResolve,
    void*,
    void*    sret,
    void*    ctx,
    uint8_t* ds,
    int      pass
) {
    if (gActive.load() && (gDhInfo || gDhTransInfo || gDhLateInfo || gDhWaterInfo) && ds) {
        void* info = *reinterpret_cast<void**>(ds + 0x148);
        if ((gDhInfo && info == gDhInfo) || (gDhTransInfo && info == gDhTransInfo)
            || (gDhLateInfo && info == gDhLateInfo) || (gDhWaterInfo && info == gDhWaterInfo)) {
            ResolvingGuard g;
            WatchGuard     wg(kWsResolve);
            void*          r = origin(sret, ctx, ds, pass);
            // Run 13 evidence: which dragon pass index each DH clone is resolved for (Opaque vs Transparent bin).
            {
                const int kind = kindOf(info);
                static std::atomic<std::uint32_t> seen[4]{};
                static constexpr char const*      kNames[4] = {
                    "DH_Terrain",
                    "DH_TerrainTransparent",
                    "DH_TerrainLate",
                    "DH_TerrainWater"
                };
                const std::uint32_t               bit = pass >= 0 && pass < 32 ? (1u << pass) : 0u;
                if (bit && !(seen[kind].fetch_or(bit) & bit))
                    DH_LOG_I("materials: DH binding first use: %s -> pass=%d", kNames[kind], pass);
            }
            // Binding struct: dragon material impl @+0x80, `bool valid` @+0x88 (RE 2026-10-19). An invalid binding
            // means the draw is skipped silently - the difference between "drawn with DH_Terrain" and "nothing".
            // Run 15: bindings resolve at ~10k/s (2026-10-20 log, apparently once per draw), so the old `n % 600`
            // sample filled the log with ~17 lines/s. Now: the first 4, then only invalid bindings (sampled); the count is
            // only kept (dhBindingResolves), not logged.
            gDhResolves[kindOf(info)].fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> logged{0}, invalidSeen{0};
            const bool              valid = sret && *(reinterpret_cast<uint8_t*>(sret) + 0x88) != 0;
            const int               n     = logged.fetch_add(1);
            const int               bad   = valid ? -1 : invalidSeen.fetch_add(1);
            if (n < 4 || bad == 0 || (bad > 0 && bad % 6000 == 0)) {
                DH_LOG_I(
                    "materials: DH binding pass=%d valid=%d impl=%p singleBlock=%d texture=%p alphaTest=%d",
                    pass,
                    (int)valid,
                    sret ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(sret) + 0x80) : nullptr,
                    (int)ds[0x11F],
                    *reinterpret_cast<void**>(ds + 0xC8),
                    (int)ds[0x120]
                );
            }
            return r;
        }
    }
    return origin(sret, ctx, ds, pass);
}

LL_STATIC_HOOK(
    HookCacheGetOrCreate,
    ll::memory::HookPriority::Normal,
    gAddrCache,
    void*,
    void*          cache,
    void**         out,
    std::uint64_t* key
) {
    if (gDhResolving.get() && key) {
        std::uint64_t k[2] = {fnv1a64("DH_Terrain"), key[1]};
        return origin(cache, out, k);
    }
    return origin(cache, out, key);
}

LL_STATIC_HOOK(
    HookEnqueueAsync,
    ll::memory::HookPriority::Normal,
    gAddrEnqueue,
    void*,
    void*        ctx,
    void*        sret,
    std::string* descName
) {
    if (gDhResolving.get() && descName && descName->rfind("MeshFallback", 0) == 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) DH_LOG_I("materials: dragon request %s -> DH_Terrain", descName->c_str());
        descName->assign("DH_Terrain");
    }
    // Breadcrumbs around the engine's async material request for our material (2026-10-19 freeze at pipeline on).
    static std::atomic<int> crumbs{0};
    const bool              ours = descName && *descName == "DH_Terrain" && crumbs.fetch_add(1) < 8;
    if (ours) DH_LOG_I("materials: enqueueAsync(DH_Terrain) enter");
    WatchGuard wg(kWsEnqueue);
    void*      r = origin(ctx, sret, descName);
    if (ours) DH_LOG_I("materials: enqueueAsync(DH_Terrain) leave");
    return r;
}

struct CompiledMaterialDefinitionOpaque;
std::atomic<bool> gForcedDhLoad{false};

LL_STATIC_HOOK(
    HookGetMaterial,
    ll::memory::HookPriority::Normal,
    gAddrGetMaterial,
    void*,
    void*              self,
    void*              sret,
    std::string const* name,
    void const*        passphrase,
    bool               forceReload
) {
    static std::atomic<int> crumbs{0};
    const bool              ours = gActive.load() && name && *name == "DH_Terrain" && crumbs.fetch_add(1) < 8;
    if (ours) DH_LOG_I("materials: getMaterial(DH_Terrain, force=%d) enter", (int)forceReload);
    void* ret;
    {
        WatchGuard wg(kWsGetMaterial);
        ret = origin(self, sret, name, passphrase, forceReload);
    }
    if (ours) DH_LOG_I("materials: getMaterial(DH_Terrain) leave");
    if (gActive.load() && name) {
        bool ok = *reinterpret_cast<void**>(sret) != nullptr;
        if (traceLoader("CompiledMaterialManager::getMaterial", *name))
            DH_LOG_I(
                "materials: trace   -> getMaterial(%s, force=%d) = %s",
                name->c_str(),
                (int)forceReload,
                ok ? "ok" : "null"
            );
        // One forced load of our material through the engine's own path (the file-open redirect serves the bytes).
        if (!gForcedDhLoad.exchange(true) && *name != "DH_Terrain") {
            std::string                                       dhName = "DH_Terrain";
            std::shared_ptr<CompiledMaterialDefinitionOpaque> dh;
            origin(self, &dh, &dhName, passphrase, false);
            DH_LOG_I(
                "materials: forced getMaterial(DH_Terrain) = %s",
                dh ? "ok (parsed)" : "null (not found or rejected)"
            );
        }
    }
    return ret;
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
    gFreezeLogPath = (mod->getModDir() / "logs" / "dh-freeze.log").wstring();
    gWatchStop.store(false);
    gWatchDone.store(false);
    gWatchThread = std::thread(watchdogMain);
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
    gAddrResolve     = resolveSig(kSigResolve, false);
    gAddrEnqueue     = resolveSig(kSigEnqueue, false);
    gAddrCache       = resolveSig(kSigCache, true);
    gAddrGetMaterial = resolveSig(kSigGetMaterial, false);
    if (gAddrGetMaterial) DH_TRACE_HOOK(HookGetMaterial);
    else DH_LOG_W("materials: getMaterial signature did not resolve; getMaterial hook skipped");
    if (gAddrResolve && gAddrEnqueue) {
        gKeyRewrite = gAddrCache != 0;
        if (gKeyRewrite) DH_TRACE_HOOK(HookCacheGetOrCreate);
        else
            DH_LOG_W(
                "materials: cacheGetOrCreate unresolved; the DH binding shares the vanilla cache entry for that "
                "variant set"
            );
        DH_TRACE_HOOK(HookEnqueueAsync);
        DH_TRACE_HOOK(HookResolveDragon);
        gBinderInstalled.store(true);
    } else {
        DH_LOG_W("materials: resolveDragonMaterial/enqueueAsync unresolved; DH_Terrain binder not installed");
    }
    DH_LOG_I("materials: injector on, DH_Terrain.material.bin %zu bytes", gMaterialBytes.size());
}

namespace {

std::string hexOf(unsigned char const* p, size_t n) {
    std::string hex;
    char        b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof(b), "%02x", p[i]);
        hex += b;
    }
    return hex;
}

enum class CloneKind { Opaque, Blended, Late, Water };

char const* cloneKindName(CloneKind k) {
    return k == CloneKind::Opaque    ? "opaque"
         : k == CloneKind::Blended   ? "transparent"
         : k == CloneKind::Late      ? "late"
                                     : "water";
}

/// Forces the render state of a DH clone. Late: like Opaque (depth write; One/Zero blending replaces the target) but
/// flagged Blending, which routes the draw into the frame graph's Transparent pass, after the stars (Run 13). Opaque: stateMask 0, depth test+write, no blend. Transparent: depth test on,
/// depth write off, Blending (stateMask bit 1), colour SourceAlpha / OneMinusSrcAlpha, alpha One / OneMinusSrcAlpha
/// (enum values from RE, see below). Layouts: DepthStencilStateDescription {depthTest u8, stencilTest u8,
/// depthFunc u8, front[4], back[4], depthWriteMask u8, ...}, BlendStateDescription {src,dst,asrc,adst,
/// colorWriteMask, enableBlend, a2c}, RasterizerStateDescription {float depthBias, float slopeScaled, cull}.
void forceDhState(mce::RenderMaterial* clone, CloneKind kind) {
    const bool transparent = kind == CloneKind::Blended || kind == CloneKind::Water;
    auto* dss = reinterpret_cast<unsigned char*>(&clone->depthStencilStateDescription);
    auto* bsd = reinterpret_cast<unsigned char*>(&clone->blendStateDescription);
    auto* rsd = reinterpret_cast<unsigned char*>(&clone->rasterizerStateDescription);
    // depthFunc: the enum is not in the SDK; take the byte from a material whose JSON says "LessEqual"
    // (cracks_overlay_tile_entity, ui3D.material) when it is loaded, else keep the source's.
    {
        static std::optional<mce::MaterialPtr> cracks;
        if (!cracks || !cracks->mRenderMaterialInfoPtr || !cracks->mRenderMaterialInfoPtr->mPtr.get())
            cracks.emplace(mce::RenderMaterialGroup::common(), HashedString{"cracks_overlay_tile_entity"});
        if (cracks->mRenderMaterialInfoPtr && cracks->mRenderMaterialInfoPtr->mPtr.get()) {
            auto const* ref = reinterpret_cast<unsigned char const*>(
                &cracks->mRenderMaterialInfoPtr->mPtr.get()->depthStencilStateDescription
            );
            DH_LOG_I("materials: cracks_overlay_tile_entity depthFunc byte=%u (JSON LessEqual)", ref[2]);
            dss[2] = ref[2];
        }
    }
    reinterpret_cast<std::uint16_t&>(clone->mStateMask) = 0;   // RenderState::None
    dss[0]                                              = 1;   // depthTestEnabled
    dss[1]                                              = 0;   // stencilTestEnabled
    dss[11]                                             = 1;   // depthWriteMask = All (D3D numbering)
    bsd[4]                                              = 0xF; // colorWriteMask RGBA
    bsd[5]                                              = 0;   // enableBlend
    bsd[6]                                              = 0;   // alpha-to-coverage
    float zero                                          = 0.0f;
    std::memcpy(rsd + 0, &zero, 4); // depthBias
    std::memcpy(rsd + 4, &zero, 4); // slopeScaledDepthBias
    if (transparent) {
        // mce::BlendTarget values from the engine's name -> enum table (static RE, built in the function at RVA
        // 0x05369840): DestColor 0, SourceColor 1, Zero 2, One 3, OneMinusDestColor 4, OneMinusSrcColor 5,
        // SourceAlpha 6, DestAlpha 7, OneMinusSrcAlpha 8. The clone source (selectionOpaqueMaterial) carries
        // DestColor/SourceColor = a 2x modulate (2 * src * dst) that turned the DH clouds cyan and opaque against the
        // sky (Run 13). The vanilla alpha-blend references were never loaded at clone time, so the factors are set
        // explicitly; a reference, when loaded, is only logged as a cross-check.
        constexpr unsigned char kOne = 3, kSourceAlpha = 6, kOneMinusSrcAlpha = 8;
        dss[11]                                             = 0;       // depth write off
        reinterpret_cast<std::uint16_t&>(clone->mStateMask) = 1u << 1; // Blending
        bsd[0]                                              = kSourceAlpha;
        bsd[1]                                              = kOneMinusSrcAlpha;
        bsd[2]                                              = kOne;
        bsd[3]                                              = kOneMinusSrcAlpha;
        bsd[5]                                              = 1;
        for (char const* name : {"entity_alphablend", "beacon_beam_transparent"}) {
            mce::MaterialPtr ref(mce::RenderMaterialGroup::common(), HashedString{name});
            if (!ref.mRenderMaterialInfoPtr || !ref.mRenderMaterialInfoPtr->mPtr.get()) continue;
            auto const* rb =
                reinterpret_cast<unsigned char const*>(&ref.mRenderMaterialInfoPtr->mPtr.get()->blendStateDescription);
            DH_LOG_I(
                "materials: blend reference %s: %s (alpha blending expects src=06 dst=08)",
                name,
                hexOf(rb, sizeof(clone->blendStateDescription)).c_str()
            );
            break;
        }
    }
    if (kind == CloneKind::Water) {
        // Run 14: the LOD translucent layer in the overdraw sky order. The opaque layer is already in the depth buffer
        // (Opaque pass); its DH_TerrainLate repeat shares the Transparent pass with this draw, which the frame graph
        // sorts back to front by an unstable per-draw distance key (ties in any order). Writing depth makes a repeat
        // that is ordered after the water fail the depth test instead of painting the sea floor over it.
        dss[11] = 1; // depth write on
    }
    if (kind == CloneKind::Late) {
        constexpr unsigned char kZero = 2, kOne = 3; // mce::BlendTarget, see the transparent branch
        reinterpret_cast<std::uint16_t&>(clone->mStateMask) = 1u << 1; // Blending (bins as Transparent)
        bsd[0]                                              = kOne;
        bsd[1]                                              = kZero;
        bsd[2]                                              = kOne;
        bsd[3]                                              = kZero;
        bsd[5]                                              = 1;
    }
    DH_LOG_I(
        "materials: DH clone forced %s: stateMask=0x%04x depthStencil=%s blend=%s depthBias=0",
        cloneKindName(kind),
        static_cast<unsigned>(reinterpret_cast<std::uint16_t&>(clone->mStateMask)),
        hexOf(dss, sizeof(clone->depthStencilStateDescription)).c_str(),
        hexOf(bsd, sizeof(clone->blendStateDescription)).c_str()
    );
}

/// Clones `src` into the switchable() info `name` (idempotent per info) and forces the state.
mce::RenderMaterialInfo* cloneDhInto(
    std::shared_ptr<mce::RenderMaterialInfo>& holder,
    char const*                               name,
    mce::RenderMaterial*                      src,
    CloneKind                                 kind
) {
    holder = mce::RenderMaterialGroup::switchable().getMaterialInfo(HashedString{name});
    if (!holder) return nullptr;
    if (!holder->mPtr.get()) {
        constexpr size_t kSize = 0x378;
        DH_LOG_I("materials: sizeof(mce::RenderMaterial)=%zu (engine 0x378)", sizeof(mce::RenderMaterial));
        void* mem = ::operator new(kSize);
        std::memset(mem, 0, kSize);
        static_cast<mce::RenderMaterial*>(mem)->$ctor(*src);
        holder->mPtr.reset(reinterpret_cast<mce::RenderMaterial*>(mem));
    }
    forceDhState(holder->mPtr.get(), kind);
    return holder.get();
}

} // namespace

bool ensureDhMceMaterial(mce::MaterialPtr const& fallback, char const* sourceName) {
    // No once-only flags: a clean reload of the switchable group (world exit / re-join, video settings) replaces its
    // RenderMaterialInfo entries with fresh ones whose mPtr is empty, so the clones have to be made again (2026-10-20:
    // after re-joining a world the LODs fell back to selectionOpaqueMaterial, magenta, for the rest of the session).
    auto live = [](char const* name) {
        auto info = mce::RenderMaterialGroup::switchable().getMaterialInfo(HashedString{name});
        return info && info->mPtr.get() != nullptr;
    };
    const bool done = live("DH_Terrain"), transDone = live("DH_TerrainTransparent"), lateDone = live("DH_TerrainLate"),
               waterDone = live("DH_TerrainWater");
    if (done && transDone && lateDone && waterDone) return true;
    if (!gBinderInstalled.load() || !fallback.mRenderMaterialInfoPtr) return false;
    mce::RenderMaterial* src = fallback.mRenderMaterialInfoPtr->mPtr.get();
    if (!src) return false;
    {
        // The clone inherits the source's render state (depth test/write, blending, culling). Log it: an overlay
        // source (selectionOpaqueMaterial) made far LOD sections draw over near ones (2026-10-19).
        auto const* dss = reinterpret_cast<unsigned char const*>(&src->depthStencilStateDescription);
        auto const* bsd = reinterpret_cast<unsigned char const*>(&src->blendStateDescription);
        auto const* rsd = reinterpret_cast<unsigned char const*>(&src->rasterizerStateDescription);
        std::string hex;
        auto        dump = [&](unsigned char const* p, size_t n) {
            char b[4];
            for (size_t i = 0; i < n; ++i) {
                snprintf(b, sizeof(b), "%02x", p[i]);
                hex += b;
            }
            hex += ' ';
        };
        dump(dss, sizeof(src->depthStencilStateDescription));
        dump(bsd, sizeof(src->blendStateDescription));
        dump(rsd, sizeof(src->rasterizerStateDescription));
        DH_LOG_I(
            "materials: DH clone source %s: stateMask=0x%04x depthStencil/blend/rasterizer bytes: %s",
            sourceName,
            static_cast<unsigned>(static_cast<std::uint16_t>(src->mStateMask)),
            hex.c_str()
        );
    }
    static std::shared_ptr<mce::RenderMaterialInfo> dhInfo, dhTransInfo, dhLateInfo, dhWaterInfo;
    if (!done) {
        if (!cloneDhInto(dhInfo, "DH_Terrain", src, CloneKind::Opaque)) return false;
        gDhInfo = dhInfo.get();
        DH_LOG_I(
            "materials: DH_Terrain mce material cloned (source info=%p, info=%p material=%p)",
            (void*)fallback.mRenderMaterialInfoPtr.get(),
            (void*)dhInfo.get(),
            (void*)dhInfo->mPtr.get()
        );
    }
    if (!transDone) {
        if (auto* ti = cloneDhInto(dhTransInfo, "DH_TerrainTransparent", src, CloneKind::Blended)) {
            gDhTransInfo = ti;
            DH_LOG_I(
                "materials: DH_TerrainTransparent mce material cloned (info=%p material=%p)",
                (void*)ti,
                (void*)ti->mPtr.get()
            );
        }
    }
    if (!lateDone) {
        if (auto* li = cloneDhInto(dhLateInfo, "DH_TerrainLate", src, CloneKind::Late)) {
            gDhLateInfo = li;
            DH_LOG_I(
                "materials: DH_TerrainLate mce material cloned (info=%p material=%p)",
                (void*)li,
                (void*)li->mPtr.get()
            );
        }
    }
    if (!waterDone) {
        if (auto* wi = cloneDhInto(dhWaterInfo, "DH_TerrainWater", src, CloneKind::Water)) {
            gDhWaterInfo = wi;
            DH_LOG_I(
                "materials: DH_TerrainWater mce material cloned (info=%p material=%p)",
                (void*)wi,
                (void*)wi->mPtr.get()
            );
        }
    }
    return true;
}

void uninstallMaterialInjector() {
    if (!gInstalled.exchange(false)) return;
    if (gActive.exchange(false)) {
        gBinderInstalled.store(false);
        if (gAddrResolve && gAddrEnqueue) {
            HookResolveDragon::unhook();
            HookEnqueueAsync::unhook();
            if (gKeyRewrite) HookCacheGetOrCreate::unhook();
        }
        if (gAddrGetMaterial) HookGetMaterial::unhook();
        HookGetFileAttributesW::unhook();
        HookGetFileAttributesExW::unhook();
        HookCreateFile2::unhook();
        HookCreateFileW::unhook();
        gWatchStop.store(true);
        // Bounded: the watchdog can be inside a (suspended-target) report; never hang the shutdown on it.
        for (int i = 0; i < 40 && !gWatchDone.load(); ++i) Sleep(50);
        if (gWatchThread.joinable()) {
            if (gWatchDone.load()) gWatchThread.join();
            else gWatchThread.detach();
        }
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

void dhBindingResolves(std::uint64_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = gDhResolves[i].load(std::memory_order_relaxed);
}

} // namespace dh::render
