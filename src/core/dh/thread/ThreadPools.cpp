#include "dh/thread/ThreadPools.h"

namespace dh {

ThreadPools::ThreadPools(ThreadOptions options, PauseConditions conditions, const IClock& clock)
: options_(options),
  conditions_(std::move(conditions)),
  picker_(options.numberOfThreads, options.runTimeRatio, clock) {
    constexpr int norm = PriorityTaskPicker::kNormPriority;

    networkCompression_ = &picker_.createExecutor("Network Compression");
    fileHandler_        = &picker_.createExecutor("IO");
    // higher priority since we want LODs to load in above all else to give the best impression of speed
    renderLoader_ = &picker_.createExecutor("Render Loader", norm + 1);
    lodBuilder_   = &picker_.createExecutor("LOD Builder");
    // the update propagator isn't necessary when moving through the world, so it is paused
    // along with the world generator when moving fast
    updatePropagator_ =
        &picker_.createExecutor("Update Propagator", norm, [this] { return updatePropagatorThreadsCanRun(); });
    worldGen_ = &picker_.createExecutor("World Gen", norm, [this] { return worldGenThreadsCanRun(); });
}

ThreadPools::~ThreadPools() {
    shutdown();
    picker_.joinAll();
    // SingleThreadExecutor destructors drain their queues and join
}

void ThreadPools::shutdown() {
    networkClientHandler_.shutdownNow();
    picker_.shutdownNow();
    beaconCulling_.shutdown();
    fullDataMigration_.shutdown();
}

bool ThreadPools::worldGenThreadsCanRun() const {
    if (cameraMovingFast()) return false;
    if (chunkBuilderOverloaded()) return false;

    if (conditions_.limitIfServerUnhealthy && conditions_.limitIfServerUnhealthy() && conditions_.serverThreadHealthy
        && !conditions_.serverThreadHealthy()) {
        return false;
    }
    return true;
}

bool ThreadPools::updatePropagatorThreadsCanRun() const {
    if (cameraMovingFast()) return false;
    if (chunkBuilderOverloaded()) return false;

    // Don't run updates if there are chunks that need processing.
    // This is especially important when running with Chunky.
    // (Java: lodBuilderExecutor != null && !isTerminated())
    if (lodBuilder_ != nullptr && !lodBuilder_->isShutdown()) {
        std::size_t maxQueueSize = static_cast<std::size_t>(lodBuilder_->poolSize()) * kPropagatorQueuePerThread;
        if (lodBuilder_->queueSize() > maxQueueSize) return false;
    }
    return true;
}

bool ThreadPools::cameraMovingFast() const {
    if (!conditions_.pauseIfMovingQuickly || !conditions_.pauseIfMovingQuickly()) return false;

    double cameraSpeed = conditions_.avgCameraSpeed ? conditions_.avgCameraSpeed() : 0.0;
    // stop these threads if moving a little bit slower than max elytra speed
    double maxAllowedSpeed = kRocketElytraSpeed - 10.0;
    return cameraSpeed > maxAllowedSpeed;
}

bool ThreadPools::chunkBuilderOverloaded() const {
    // rarely the builder executor can queue up thousands of tasks, if this happens we want
    // to limit other executors so these tasks can be handled.
    return lodBuilder_ != nullptr && lodBuilder_->queueSize() > kChunkBuilderOverloadedQueueSize;
}

} // namespace dh
