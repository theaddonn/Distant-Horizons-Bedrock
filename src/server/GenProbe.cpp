#include "bedrock_common/GenProbe.h"

#include "bedrock_common/FileLogger.h"

#include "ll/api/memory/Hook.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/levelgen/v1/OverworldGenerator.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace dh::bedrock::probe {

namespace {

long long nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

LL_TYPE_INSTANCE_HOOK(
    ProbeLoadChunkHook,
    ll::memory::HookPriority::Normal,
    OverworldGenerator,
    &OverworldGenerator::$loadChunk,
    void,
    ::LevelChunk& lc,
    bool          force
) {
    const bool      ours = gOurs.get();
    const long long t0   = nowUs();
    origin(lc, force);
    record(kLoad, GetCurrentThreadId(), nowUs() - t0, ours, true);
}

LL_TYPE_INSTANCE_HOOK(
    ProbeStructHook,
    ll::memory::HookPriority::Normal,
    OverworldGenerator,
    &OverworldGenerator::$structurePostProcessChunk,
    bool,
    ::ChunkViewSource& v
) {
    const bool      ours = gOurs.get();
    const long long t0   = nowUs();
    const bool      r    = origin(v);
    record(kStruct, GetCurrentThreadId(), nowUs() - t0, ours, r);
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    ProbeDecoHook,
    ll::memory::HookPriority::Normal,
    OverworldGenerator,
    &OverworldGenerator::$decorationPostProcessChunk,
    bool,
    ::ChunkViewSource& v
) {
    const bool      ours = gOurs.get();
    const long long t0   = nowUs();
    const bool      r    = origin(v);
    record(kDeco, GetCurrentThreadId(), nowUs() - t0, ours, r);
    return r;
}

const char* kKindName[kKinds]   = {"loadChunk", "structurePostProcess", "decorationPostProcess"};
const char* kPhaseName[kPhases] = {
    "hop(LevelChunk ctor on server thread)",
    "loadChunk (pure)",
    "cell mutex wait",
    "baseCell total (per call)",
    "decoMutex wait",
    "snapshot+countNonAir+freeze",
    "lightBlockOnly",
    "structureCell (per cell)",
    "decorateCell (per cell)",
    "decorateAndDeliver total (per batch)",
    "runFeatures wall (per task)",
    "hop: executor queue wait",
    "hop: LevelChunk ctor run (server thread)",
    "server tick gap",
    "driver onServerTick duration"
};

std::mutex              gMutex;
std::condition_variable gCv;
bool                    gStop = false, gStarted = false;
std::thread             gThread;
bool                    gHooked[3] = {false, false, false};

void report(std::uint64_t from, std::uint64_t to, double wallS, long long (&lastUs)[kPhases], long long (&lastN)[kPhases],
            long long& lastCellsGen, long long& lastCellsReq, long long& lastDelivered) {
    if (to - from > kRing) from = to - kRing;
    struct Bucket {
        std::vector<std::uint32_t> us;
        std::set<std::uint32_t>    tids;
        double                     sumMs = 0;
    };
    Bucket b[kKinds][2];
    std::uint64_t falseCalls[kKinds][2] = {};
    for (std::uint64_t i = from; i < to; ++i) {
        Rec& r = gRing[i & (kRing - 1)];
        if (!r.valid.load(std::memory_order_acquire)) continue;
        const std::uint32_t k = r.kind.load(), o = r.ours.load() & 1, okv = (r.ours.load() >> 1) & 1;
        if (k >= kKinds) continue;
        if (!okv) { // post-process call that returned false (scheduler poll, not ready / nothing done): count only
            ++falseCalls[k][o];
            continue;
        }
        b[k][o].us.push_back(r.us.load());
        b[k][o].tids.insert(r.tid.load());
        b[k][o].sumMs += r.us.load() / 1000.0;
    }
    for (int k = 0; k < kKinds; ++k)
        for (int o = 0; o < 2; ++o) {
            auto& x = b[k][o];
            if (falseCalls[k][o] > 0)
                DH_LOG_I("gen probe %-7s %-22s returned-false polls: %llu", o ? "OURS" : "VANILLA", kKindName[k],
                         static_cast<unsigned long long>(falseCalls[k][o]));
            if (x.us.empty()) continue;
            std::sort(x.us.begin(), x.us.end());
            const auto n = x.us.size();
            DH_LOG_I(
                "gen probe %-6s %-22s n=%zu (%.1f/s) avg=%.2f ms p50=%.2f p95=%.2f max=%.2f threads=%zu busy=%.2f cores",
                o ? "OURS" : "VANILLA",
                kKindName[k],
                n,
                n / wallS,
                x.sumMs / n,
                x.us[n / 2] / 1000.0,
                x.us[std::min(n - 1, static_cast<std::size_t>(n * 0.95))] / 1000.0,
                x.us.back() / 1000.0,
                x.tids.size(),
                x.sumMs / 1000.0 / wallS
            );
        }
    // our generator's phases
    const long long cg = gCellsGenerated.load(), cr = gCellsRequested.load(), dl = gDelivered.load();
    if (dl - lastDelivered > 0 || cg - lastCellsGen > 0) {
        DH_LOG_I(
            "gen probe OURS cells: loadChunk ran %lld, baseCell calls %lld, chunks delivered %lld => %.2f cells/delivered chunk",
            cg - lastCellsGen,
            cr - lastCellsReq,
            dl - lastDelivered,
            dl - lastDelivered > 0 ? double(cg - lastCellsGen) / double(dl - lastDelivered) : 0.0
        );
        for (int p = 0; p < kPhases; ++p) {
            const long long us = gPhaseUs[p].load(), n = gPhaseN[p].load();
            const long long dus = us - lastUs[p], dn = n - lastN[p];
            if (dn > 0)
                DH_LOG_I(
                    "gen probe OURS phase %-38s n=%lld sum=%.0f ms avg=%.2f ms",
                    kPhaseName[p],
                    dn,
                    dus / 1000.0,
                    dus / 1000.0 / dn
                );
        }
    }
    for (int p = 0; p < kPhases; ++p) {
        lastUs[p] = gPhaseUs[p].load();
        lastN[p]  = gPhaseN[p].load();
    }
    lastCellsGen  = cg;
    lastCellsReq  = cr;
    lastDelivered = dl;
}

void reporterLoop() {
    std::uint64_t last = gRingHead.load();
    long long     lastUs[kPhases] = {}, lastN[kPhases] = {}, lcg = 0, lcr = 0, ldl = 0;
    auto          lastT = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::unique_lock lock(gMutex);
            if (gCv.wait_for(lock, std::chrono::seconds(10), [] { return gStop; })) return;
        }
        const auto          now  = std::chrono::steady_clock::now();
        const double        wall = std::chrono::duration<double>(now - lastT).count();
        const std::uint64_t head = gRingHead.load();
        lastT                    = now;
        try {
            report(last, head, wall, lastUs, lastN, lcg, lcr, ldl);
        } catch (...) {}
        last = head;
    }
}

} // namespace

void start() {
    if (!enabled()) return;
    std::lock_guard lock(gMutex);
    if (gStarted) return;
    gStarted = true;
    gStop    = false;
    gHooked[0] = ProbeLoadChunkHook::hook() == 0;
    gHooked[1] = ProbeStructHook::hook() == 0;
    gHooked[2] = ProbeDecoHook::hook() == 0;
    DH_LOG_I(
        "gen probe: enabled (hooks loadChunk=%d structurePostProcessChunk=%d decorationPostProcessChunk=%d), summary every 10 s",
        gHooked[0] ? 1 : 0,
        gHooked[1] ? 1 : 0,
        gHooked[2] ? 1 : 0
    );
    gThread = std::thread(reporterLoop);
}

void stop() {
    {
        std::lock_guard lock(gMutex);
        if (!gStarted) return;
        gStop = true;
    }
    gCv.notify_all();
    if (gThread.joinable()) gThread.join();
    if (gHooked[0]) ProbeLoadChunkHook::unhook();
    if (gHooked[1]) ProbeStructHook::unhook();
    if (gHooked[2]) ProbeDecoHook::unhook();
    std::lock_guard lock(gMutex);
    gStarted = false;
}

} // namespace dh::bedrock::probe
