#pragma once

// World-generation timing probe (env DH_GEN_PROBE=1; off by default). Measurement only.
//   * server DLL: src/server/GenProbe.cpp hooks OverworldGenerator::$loadChunk / $structurePostProcessChunk /
//     $decorationPostProcessChunk, records (kind, thread, us, ours?) into a lock-free ring and logs a summary every 10 s.
//   * BdsWorldGenerator adds fine-grained phase timers (probe::add) that the same reporter prints.
// Everything here is atomics only; nothing allocates inside a hook.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace dh::bedrock::probe {

/// Per-thread flag in a dynamic TLS slot (NOT thread_local, see TlsFlag in client/render/MaterialInjector.cpp).
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

inline TlsFlag gOurs; ///< set around our own gen->loadChunk calls

inline bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("DH_GEN_PROBE");
        return e != nullptr && std::string(e) == "1";
    }();
    return on;
}
/// Also drive our generator standalone from the server driver (no client needed): DH_GEN_PROBE_OURS=<batches>.
inline int oursBatches() {
    static const int n = [] {
        const char* e = std::getenv("DH_GEN_PROBE_OURS");
        return e != nullptr ? std::atoi(e) : 0;
    }();
    return n;
}

/// Probe only: skip the server-thread hop for scratch LevelChunk construction (headless BDS does not tick without players).
inline bool noHop() {
    static const bool on = [] {
        const char* e = std::getenv("DH_GEN_PROBE_NOHOP");
        return enabled() && e != nullptr && std::string(e) == "1";
    }();
    return on;
}

/// Probe only (with DH_GEN_PROBE_NOHOP=1): DH_GEN_PROBE_HOPSIM_MS=N emulates the tick-gated hop by sleeping to the next
/// multiple of N ms on the steady clock before constructing the chunk (all waiters wake together, like one executor drain).
inline int hopSimMs() {
    static const int n = [] {
        const char* e = std::getenv("DH_GEN_PROBE_HOPSIM_MS");
        return e != nullptr ? std::atoi(e) : 0;
    }();
    return n;
}

enum Kind { kLoad = 0, kStruct = 1, kDeco = 2, kKinds };

/// Phases of OUR generator (BdsWorldGenerator). Times are microseconds summed over calls.
enum Phase {
    pHop,       ///< onServerThread(LevelChunk ctor): queue wait + ctor
    pLoad,      ///< pure gen->loadChunk (+ tryChangeState)
    pCellWait,  ///< blocked on Batch::Cell::m (another task generating that cell, incl. its hop)
    pCellTotal, ///< whole baseCell() call incl. wait (cells that already existed are cheap)
    pDecoWait,  ///< blocked on Batch::decoMutex
    pSnap,      ///< snap() + countNonAir + freeze in decorateAndDeliver / generateScratch
    pLight,     ///< lightBlockOnly
    pStruct,    ///< structureCell, per cell
    pDeco,      ///< decorateCell, per cell
    pFeatBatch, ///< decorateAndDeliver total (struct + deco + snapshots + delivery)
    pTask,      ///< runFeatures wall per task (base terrain of 5x5 incl. waiting)
    pHopQueue,  ///< enqueue -> start on the server thread (executor queue wait)
    pHopRun,    ///< LevelChunk ctor itself, measured on the server thread
    pTickGap,   ///< time between two ServerLevelTick events seen by the driver
    pTickMine,  ///< time the driver spends inside onServerTick
    kPhases
};
inline std::atomic<long long> gPhaseUs[kPhases], gPhaseN[kPhases];
inline std::atomic<long long> gCellsGenerated{0}; ///< cells that actually ran loadChunk
inline std::atomic<long long> gCellsRequested{0}; ///< baseCell calls
inline std::atomic<long long> gDelivered{0};      ///< chunks delivered by decorateAndDeliver / generateScratch

inline void add(Phase p, long long us) {
    gPhaseUs[p].fetch_add(us, std::memory_order_relaxed);
    gPhaseN[p].fetch_add(1, std::memory_order_relaxed);
}

struct Timer {
    std::chrono::steady_clock::time_point t0;
    bool                                  on;
    Phase                                 p;
    explicit Timer(Phase ph)
    : t0(enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}),
      on(enabled()),
      p(ph) {}
    long long us() const {
        return on ? std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()
                  : 0;
    }
    void stop() {
        if (on) add(p, us());
        on = false;
    }
    ~Timer() { stop(); }
};

/// Records of the engine hooks (server DLL).
struct Rec {
    std::atomic<std::uint32_t> kind{0}, tid{0}, us{0}, ours{0}, valid{0};
};
inline constexpr std::uint32_t    kRing = 1u << 16;
inline Rec                        gRing[kRing];
inline std::atomic<std::uint64_t> gRingHead{0};

inline void record(Kind k, std::uint32_t tid, long long us, bool ours, bool ok) {
    const std::uint64_t i = gRingHead.fetch_add(1, std::memory_order_relaxed);
    Rec&                r = gRing[i & (kRing - 1)];
    r.kind.store(k, std::memory_order_relaxed);
    r.tid.store(tid, std::memory_order_relaxed);
    r.us.store(static_cast<std::uint32_t>(us), std::memory_order_relaxed);
    r.ours.store((ours ? 1 : 0) | (ok ? 2 : 0), std::memory_order_relaxed);
    r.valid.store(1, std::memory_order_release);
}

/// Server DLL: install the hooks and start the 10 s reporter (idempotent); stop on disable.
void start();
void stop();

} // namespace dh::bedrock::probe
