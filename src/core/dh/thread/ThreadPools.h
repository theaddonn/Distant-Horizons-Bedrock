#pragma once

// Port of util/threading/ThreadPoolUtil.java: the named DH executors.
//
// Java kept them in statics; here ThreadPools is an owned object so tests and
// the client/server glue can have several. Picker-backed executors (share the
// `numberOfThreads` budget): "Network Compression", "IO" (fileHandler),
// "Render Loader" (priority +1), "LOD Builder", "Update Propagator" (paused by
// updatePropagatorThreadsCanRun), "World Gen" (paused by worldGenThreadsCanRun).
// Standalone single-thread executors: cleanup ("Cleanup", never shut down by
// shutdown() just like Java - it is drained by the destructor), "Beacon Culling",
// "Network Client Handler", "Full Data Migration".
//
// Config / game state the Java predicates read are injected through
// PauseConditions (empty function = Java default noted per field).

#include "dh/platform/IClock.h"
#include "dh/thread/PriorityTaskPicker.h"
#include "dh/thread/SingleThreadExecutor.h"

#include <functional>

namespace dh {

struct ThreadOptions {
    int   numberOfThreads = 4;    // Config.Common.MultiThreading.numberOfThreads
    float runTimeRatio    = 1.0f; // Config.Common.MultiThreading.threadRunTimeRatio
};

struct PauseConditions {
    /// Config.Common.WorldGenerator.pauseIfMovingQuickly (default: false)
    std::function<bool()> pauseIfMovingQuickly;
    /// ClientApi.INSTANCE.getAvgCameraSpeed(), blocks/s (default: 0)
    std::function<double()> avgCameraSpeed;
    /// Config.Common.WorldGenerator.limitIfServerUnhealthy (default: false)
    std::function<bool()> limitIfServerUnhealthy;
    /// MC_SHARED.isServerThreadHealthy() (default: true)
    std::function<bool()> serverThreadHealthy;
};

class ThreadPools {
public:
    /// LodUtil.ROCKET_ELYTRA_SPEED_IN_BLOCKS_PER_SEC
    static constexpr double kRocketElytraSpeed = 30.0;
    /// chunkBuilderOverloaded(): LOD builder queue above this pauses world gen and the propagator
    static constexpr std::size_t kChunkBuilderOverloadedQueueSize = 1000;
    /// updatePropagatorThreadsCanRun(): pauses if lodBuilder queue > poolSize * this
    static constexpr std::size_t kPropagatorQueuePerThread = 200;

    explicit ThreadPools(
        ThreadOptions   options    = {},
        PauseConditions conditions = {},
        const IClock&   clock      = PriorityTaskPicker::defaultClock()
    );
    ~ThreadPools();

    ThreadPools(const ThreadPools&)            = delete;
    ThreadPools& operator=(const ThreadPools&) = delete;

    Executor& fileHandler() { return *fileHandler_; }
    Executor& renderLoader() { return *renderLoader_; }
    Executor& lodBuilder() { return *lodBuilder_; }
    Executor& updatePropagator() { return *updatePropagator_; }
    Executor& worldGen() { return *worldGen_; }
    Executor& networkCompression() { return *networkCompression_; }

    SingleThreadExecutor& cleanup() { return cleanup_; }
    SingleThreadExecutor& beaconCulling() { return beaconCulling_; }
    SingleThreadExecutor& networkClientHandler() { return networkClientHandler_; }
    SingleThreadExecutor& fullDataMigration() { return fullDataMigration_; }

    PriorityTaskPicker& picker() { return picker_; }

    bool worldGenThreadsCanRun() const;
    bool updatePropagatorThreadsCanRun() const;

    /// Java shutdownThreadPools(): client handler + picker shutdownNow, beacon + migration
    /// shutdown (queued work drains). Cleanup keeps running until the destructor.
    void shutdown();

private:
    bool cameraMovingFast() const;
    bool chunkBuilderOverloaded() const;

    ThreadOptions      options_;
    PauseConditions    conditions_;
    PriorityTaskPicker picker_;

    Executor* networkCompression_ = nullptr;
    Executor* fileHandler_        = nullptr;
    Executor* renderLoader_       = nullptr;
    Executor* lodBuilder_         = nullptr;
    Executor* updatePropagator_   = nullptr;
    Executor* worldGen_           = nullptr;

    SingleThreadExecutor cleanup_{"Cleanup"};
    SingleThreadExecutor beaconCulling_{"Beacon Culling"};
    SingleThreadExecutor networkClientHandler_{"Network Client Handler"};
    SingleThreadExecutor fullDataMigration_{"Full Data Migration"};
};

} // namespace dh
