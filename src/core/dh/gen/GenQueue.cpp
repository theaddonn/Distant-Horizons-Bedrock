#include "dh/gen/GenQueue.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/ChunkPos.h"
#include "dh/pos/LodUtil.h"
#include "dh/pos/SectionPos.h"
#include "dh/util/Assert.h"
#include "dh/util/Bits.h"

namespace dh {

namespace {

/// Port of util/objects/RollingAverage.java, quirk included: `currentSize` is
/// max(index + 1, currentSize) *after* the index advance, so the first sample
/// is divided by 2 and the divisor stays one ahead of the sample count until the
/// buffer wraps. Only feeds the HUD's chunks/sec estimate.
class RollingAverage {
public:
    explicit RollingAverage(int size) : values_(static_cast<std::size_t>(size <= 0 ? 1 : size), 0.0) {}

    void add(double value) {
        sum_            -= values_[index_];
        values_[index_]  = value;
        sum_            += value;
        index_           = (index_ + 1) % values_.size();
        currentSize_     = std::max(index_ + 1, currentSize_);
        ++lifetimeCount_;
    }
    double       average() const { return sum_ / static_cast<double>(currentSize_); }
    std::int64_t lifetimeCount() const { return lifetimeCount_; }

private:
    std::vector<double> values_;
    double              sum_           = 0.0;
    std::size_t         index_         = 0;
    std::size_t         currentSize_   = 0;
    std::int64_t        lifetimeCount_ = 0;
};

/// Java generation/tasks/DataSourceRetrievalTask.java
struct Task {
    std::uint64_t                    pos;
    std::int8_t                      requestDetailLevel;
    std::int32_t                     widthInChunks;
    std::shared_ptr<RetrievalFuture> future = std::make_shared<RetrievalFuture>();

    Task(std::uint64_t pos_, std::int8_t dataDetail)
    : pos(pos_),
      requestDetailLevel(dataDetail),
      // int widthInBlocks = BitShiftUtil.powerOfTwo(detail); widthInChunks = widthInBlocks / CHUNK_WIDTH
      widthInChunks(jdiv(powerOfTwo(sec::detail(pos_)), lod::CHUNK_WIDTH)) {}
};

bool isInterruptOrReject(const Error& e) { return e.code == ErrorCode::Cancelled || e.code == ErrorCode::Rejected; }

} // namespace

//======//
// Impl //
//======//

struct GenQueue::Impl : std::enable_shared_from_this<GenQueue::Impl> {
    IWorldGenerator& generator;
    IExecutor&       queueThread;
    IExecutor&       genPool;
    IClock&          clock;

    mutable std::mutex mutex;

    // Java: ConcurrentHashMap<Long, DataSourceRetrievalTask>; std::map so every
    // iteration (selection, cancellation, counting) is in ascending pos order.
    std::map<std::uint64_t, std::shared_ptr<Task>> waitingTaskByPos;
    std::map<std::uint64_t, std::shared_ptr<Task>> inProgressGenTasksByLodPos;

    BlockPos2D     generationTargetPos{};
    int            numThreads;
    GenerationMode generatorMode = GenerationMode::Features;

    bool generationQueueRunning = false;
    bool rerunRequested         = false; ///< a restart was asked for while the loop was running
    bool closing                = false; ///< Java: generatorClosingFuture != null
    bool deferGeneratorClose    = false; ///< shutdown(false) with tasks still running
    bool generatorClosed        = false;

    RollingAverage rollingAverageChunkGenTimeInMs;

    Impl(IWorldGenerator& gen, IExecutor& queueExec, IExecutor& pool, int threads, IClock& clk)
    : generator(gen),
      queueThread(queueExec),
      genPool(pool),
      clock(clk),
      numThreads(threads),
      rollingAverageChunkGenTimeInMs(std::max(threads, 1) * 500) {}

    //===============//
    // task handling //
    //===============//

    // Java :125-172
    std::shared_ptr<RetrievalFuture> submitRetrievalTask(std::uint64_t pos, std::uint8_t requiredDataDetailIn) {
        // generator queries happen outside the lock (they may be arbitrarily slow / foreign code)
        const int smallest = generator.smallestDataDetailLevel();
        const int largest  = generator.largestDataDetailLevel();

        std::lock_guard lock(mutex);

        // the generator is shutting down, don't add new tasks (:129)
        if (closing) return rejectedFuture("queue is shutting down");

        // use the existing task if present (:137)
        if (auto it = waitingTaskByPos.find(pos); it != waitingTaskByPos.end()) return it->second->future;

        // make sure the generator can provide the requested position (:148)
        int requiredDataDetail = requiredDataDetailIn;
        if (requiredDataDetail < smallest) {
            // Java: throw new UnsupportedOperationException(...)
            return rejectedFuture("Current generator does not meet requiredDataDetail level");
        }
        if (requiredDataDetail > largest) requiredDataDetail = largest;

        // the request should be at least chunk-sized (:158)
        const bool chunkSized = sec::detail(pos) > requiredDataDetail + lod::CHUNK_DETAIL_LEVEL;
        DH_ASSERT(chunkSized, "world gen request smaller than a chunk");
        if (!chunkSized) return rejectedFuture("request smaller than a chunk");

        auto task = std::make_shared<Task>(pos, static_cast<std::int8_t>(requiredDataDetail));
        waitingTaskByPos.emplace(pos, task);
        return task->future;
    }

    static std::shared_ptr<RetrievalFuture> rejectedFuture(std::string reason) {
        auto future = std::make_shared<RetrievalFuture>();
        future->complete(RetrievalResult::rejected(std::move(reason)));
        return future;
    }

    // Java :174-189. The predicate runs outside the lock; a task that gets started in between
    // is simply not found anymore (same as Java's remove() returning null).
    void removeRetrievalRequestIf(const std::function<bool(std::uint64_t)>& removeIf) {
        std::vector<std::uint64_t> keys;
        {
            std::lock_guard lock(mutex);
            keys.reserve(waitingTaskByPos.size());
            for (const auto& [pos, task] : waitingTaskByPos) keys.push_back(pos);
        }

        std::vector<std::uint64_t> matching;
        for (std::uint64_t pos : keys) {
            if (removeIf(pos)) matching.push_back(pos);
        }
        if (matching.empty()) return;

        std::vector<std::shared_ptr<RetrievalFuture>> removed;
        {
            std::lock_guard lock(mutex);
            for (std::uint64_t pos : matching) {
                auto it = waitingTaskByPos.find(pos);
                if (it == waitingTaskByPos.end()) continue;
                removed.push_back(it->second->future);
                waitingTaskByPos.erase(it);
            }
        }
        // cancel tasks so any waiting future steps can be triggered (:185)
        for (auto& future : removed) future->cancel();
    }

    //===============//
    // running tasks //
    //===============//

    // Java :221-229
    void startAndSetTargetPos(BlockPos2D targetPos) {
        {
            std::lock_guard lock(mutex);
            generationTargetPos = targetPos;
        }
        tryQueueNewWorldGenRequestsAsync();
    }

    // Java :230-270 (synchronized)
    void tryQueueNewWorldGenRequestsAsync() {
        {
            std::lock_guard lock(mutex);
            if (closing) return;
            if (generationQueueRunning) {
                // the running loop re-checks for work before it gives up the flag
                rerunRequested = true;
                return;
            }
            generationQueueRunning = true;
        }
        postQueueLoop();
    }

    void postQueueLoop() {
        std::weak_ptr<Impl> weak = weak_from_this();
        const bool          ok   = queueThread.execute([weak] {
            if (auto self = weak.lock()) self->runQueueLoop();
        });
        if (!ok) {
            std::lock_guard lock(mutex);
            generationQueueRunning = false;
            rerunRequested         = false;
        }
    }

    /// Body of the Java lambda at :247-269.
    void runQueueLoop() {
        bool closingNow;
        {
            std::lock_guard lock(mutex);
            closingNow = closing;
        }
        if (!closingNow) {
            generator.preGeneratorTaskStart();

            // queue generation tasks until the generator is full, or there are no more tasks to generate
            bool taskStarted = true;
            while (!isGeneratorBusy() && taskStarted) {
                taskStarted = tryStartNextWorldGenTask();
            }
        }

        // finally { this.generationQueueRunning = false; }
        bool repost = false;
        {
            std::lock_guard lock(mutex);
            if (rerunRequested && !closing) {
                rerunRequested = false;
                repost         = true; // keep generationQueueRunning = true
            } else {
                generationQueueRunning = false;
                rerunRequested         = false;
            }
        }
        if (repost) postQueueLoop();
    }

    // Java :271-283
    bool isGeneratorBusy() const {
        std::size_t inProgress;
        int         threads;
        {
            std::lock_guard lock(mutex);
            inProgress = inProgressGenTasksByLodPos.size();
            threads    = numThreads;
        }
        // queue more tasks if any of the threads are available
        const int worldGenThreadCount = std::max(threads, 1);
        if (inProgress > static_cast<std::size_t>(worldGenThreadCount)) return true;
        return generator.isBusy();
    }

    /// Java :288-368. @return false if no tasks were found to generate
    bool tryStartNextWorldGenTask() {
        const int smallest = generator.smallestDataDetailLevel();
        const int largest  = generator.largestDataDetailLevel();

        std::shared_ptr<Task> closestTask;
        std::shared_ptr<Task> existingTask;
        bool                  canGenerate = false;
        GenerationMode        mode        = GenerationMode::Features;
        {
            std::lock_guard lock(mutex);
            if (waitingTaskByPos.empty()) return false;

            // find the closest task (:297-310). Java: reduceEntries with
            // `(a.dist < b.dist) ? a : b`, so a tie goes to whichever entry the
            // parallel reduction happened to visit last. Here: strict `<` while
            // walking ascending pos => the tied task with the smallest pos wins.
            const BlockPos2D targetPos    = generationTargetPos;
            std::int32_t     closestDist  = 0;
            auto             closestEntry = waitingTaskByPos.end();
            for (auto it = waitingTaskByPos.begin(); it != waitingTaskByPos.end(); ++it) {
                const std::int32_t distance = taskDistance(it->first, targetPos);
                if (closestEntry == waitingTaskByPos.end() || distance < closestDist) {
                    closestDist  = distance;
                    closestEntry = it;
                }
            }
            closestTask = closestEntry->second;

            // remove the task we found, we are going to start it and don't want to run it multiple times (:320)
            waitingTaskByPos.erase(closestEntry);

            // do we need to modify this task to generate it? (:323)
            canGenerate = canGenerateDetailLevel(sec::detail(closestTask->pos), smallest, largest);
            if (canGenerate) {
                auto it = inProgressGenTasksByLodPos.find(closestTask->pos);
                if (it == inProgressGenTasksByLodPos.end()) {
                    // no task exists for this position, start one.
                    // (Java inserts inside startGenerationEvent :439; done here so the
                    // busy accounting and the start are atomic.)
                    inProgressGenTasksByLodPos.emplace(closestTask->pos, closestTask);
                    mode = generatorMode;
                } else {
                    existingTask = it->second;
                }
            }
        }

        if (!canGenerate) {
            // detail level is too high (if the detail level was too low, the generator
            // would've ignored the request), split up the task (:361)
            closestTask->future->complete(RetrievalResult::split());
            return true;
        }

        if (existingTask) {
            // shouldn't normally happen, but if we somehow queued the same task twice:
            // merge the two futures so they both complete (:339-353)
            std::shared_ptr<RetrievalFuture> mergedFuture = closestTask->future;
            existingTask->future->onComplete([mergedFuture](const RetrievalResult& result) {
                mergedFuture->complete(result);
            });
            return true;
        }

        startWorldGenTaskGroup(closestTask, mode);

        // a task has been started or queued, queue another task
        return true;
    }

    // Java :302-303
    static std::int32_t taskDistance(std::uint64_t pos, BlockPos2D targetPos) {
        std::int32_t      distance    = sec::centerBlockPos(pos).chebyshevDist(targetPos);
        const std::int8_t detailLevel = sec::detail(pos);
        // generate low detail LODs first. detail 0 is unreachable (submit requires detail > 4);
        // Java would throw ArithmeticException there.
        if (detailLevel != 0) distance = jdiv(distance, detailLevel);
        return distance;
    }

    // Java :369-374
    static bool canGenerateDetailLevel(std::int8_t taskDetailLevel, int smallest, int largest) {
        const int requestedDetailLevel =
            static_cast<std::int8_t>(static_cast<int>(taskDetailLevel) - sec::SECTION_MINIMUM_DETAIL_LEVEL);
        return smallest <= requestedDetailLevel && requestedDetailLevel <= largest;
    }

    // Java :375-436 + startGenerationEvent :437-465 + startApiDataSourceGenerationEvent :600-641
    void startWorldGenTaskGroup(const std::shared_ptr<Task>& worldGenTask, GenerationMode mode) {
        const std::uint64_t taskPos               = worldGenTask->pos;
        const std::int64_t  generationStartMsTime = clock.nowMillis();

        const ChunkPos chunkPosMin(BlockPos2D{sec::minCornerBlockX(taskPos), sec::minCornerBlockZ(taskPos)});

        GenerateRequest request;
        request.sectionPos  = taskPos;
        request.minChunkX   = chunkPosMin.x;
        request.minChunkZ   = chunkPosMin.z;
        request.widthChunks = worldGenTask->widthInChunks;
        request.dataDetail =
            static_cast<std::int8_t>(static_cast<int>(sec::detail(taskPos)) - sec::SECTION_MINIMUM_DETAIL_LEVEL);
        request.mode = mode;

        // shutdown() may have run between the pick and this point: never hand work to a
        // generator that has been (or is about to be) closed. The task's future was already
        // cancelled by shutdown(); give its slot back and, if that was the last lingering slot
        // of a deferred close, close the generator.
        {
            bool closeGeneratorNow = false;
            bool abort             = false;
            {
                std::lock_guard lock(mutex);
                if (closing) {
                    abort   = true;
                    auto it = inProgressGenTasksByLodPos.find(taskPos);
                    if (it != inProgressGenTasksByLodPos.end() && it->second == worldGenTask) {
                        inProgressGenTasksByLodPos.erase(it);
                    }
                    if (deferGeneratorClose && !generatorClosed && inProgressGenTasksByLodPos.empty()) {
                        closeGeneratorNow = true;
                        generatorClosed   = true;
                    }
                }
            }
            if (abort) {
                worldGenTask->future->cancel();
                if (closeGeneratorNow) generator.close();
                return;
            }
        }

        std::weak_ptr<Impl> weak = weak_from_this();
        generator.generate(
            request,
            genPool,
            IWorldGenerator::ChunkCallback{}, // Java: (IDhApiFullDataSource apiDataSource) -> { }
            [weak, worldGenTask, generationStartMsTime](Result<std::shared_ptr<FullDataSource>> result) {
                if (auto self = weak.lock()) {
                    self->onGenerationDone(worldGenTask, generationStartMsTime, std::move(result));
                }
            }
        );
    }

    /// Java :388-435 (the thenRun timing block and the handle() block).
    void onGenerationDone(
        const std::shared_ptr<Task>&            worldGenTask,
        std::int64_t                            generationStartMsTime,
        Result<std::shared_ptr<FullDataSource>> result
    ) {
        const std::uint64_t taskPos = worldGenTask->pos;

        bool taskRemoved;
        bool closeGeneratorNow = false;
        {
            std::lock_guard lock(mutex);

            // calculate generation speed (:388-394; thenRun only fires on normal completion)
            if (result.has_value()) {
                const std::int64_t totalGenTimeInMs = clock.nowMillis() - generationStartMsTime;
                const std::int32_t chunkCount       = wmul(worldGenTask->widthInChunks, worldGenTask->widthInChunks);
                const double timePerChunk = static_cast<double>(totalGenTimeInMs) / static_cast<double>(chunkCount);
                rollingAverageChunkGenTimeInMs.add(timePerChunk);
            }

            // failed tasks also need to free their slot, otherwise isGeneratorBusy()
            // will eventually always return true and this level will stop generating (:402)
            auto it     = inProgressGenTasksByLodPos.find(taskPos);
            taskRemoved = it != inProgressGenTasksByLodPos.end() && it->second == worldGenTask;
            if (taskRemoved) inProgressGenTasksByLodPos.erase(it);

            if (closing && deferGeneratorClose && !generatorClosed && inProgressGenTasksByLodPos.empty()) {
                closeGeneratorNow = true;
                generatorClosed   = true;
            }
        }

        if (!result.has_value()) {
            const Error& error = result.error();
            // don't log the shutdown exceptions (:407)
            if (!isInterruptOrReject(error)) {
                log::error("Error generating data for pos: " + sec::toString(taskPos) + ": " + error.message);
            }
            worldGenTask->future->complete(
                isInterruptOrReject(error) ? RetrievalResult::rejected(error.message)
                                           : RetrievalResult::failed(error.message)
            );
        } else if (!result.value()) {
            // Java: fullDataSource.recordLastSeen() -> NPE -> caught at :424 -> completeExceptionally
            log::error(
                "Unexpected error completing world gen task at pos: [" + sec::toString(taskPos) + "]: null data source"
            );
            worldGenTask->future->complete(RetrievalResult::failed("generator completed without a data source"));
        } else {
            DH_ASSERT(taskRemoved, "Unable to find in progress generator task for completed position");
            worldGenTask->future->complete(RetrievalResult::success(taskPos, std::move(result.value())));
        }

        // finally (:429-432)
        if (closeGeneratorNow) generator.close();
        tryQueueNewWorldGenRequestsAsync();
    }

    //==========//
    // shutdown //
    //==========//

    /// Java startClosingAsync(true, interruptRunning) :725-767 followed by close() :770-810.
    void shutdown(bool interruptRunning) {
        std::vector<std::shared_ptr<RetrievalFuture>> toCancel;
        bool                                          closeGeneratorNow = false;
        {
            std::lock_guard lock(mutex);
            if (closing) {
                // Already shutting down: the futures were cancelled by the first call, but a
                // shutdown(false) may still be waiting for lingering tasks before closing the
                // generator. Java close() always reaches generator.close() (:795), so an
                // interrupting call (or the destructor) closes it now.
                if (interruptRunning && !generatorClosed) {
                    generatorClosed     = true;
                    deferGeneratorClose = false;
                    closeGeneratorNow   = true;
                }
                if (!closeGeneratorNow) return;
            } else {
                closing = true;

                // stop and remove any in progress tasks (:733; Java order: in-progress first, then waiting)
                for (const auto& [pos, task] : inProgressGenTasksByLodPos) toCancel.push_back(task->future);
                for (const auto& [pos, task] : waitingTaskByPos) toCancel.push_back(task->future);
                waitingTaskByPos.clear();

                if (interruptRunning || inProgressGenTasksByLodPos.empty()) {
                    closeGeneratorNow = true;
                    generatorClosed   = true;
                } else {
                    // let any lingering generation requests finish before fully shutting down
                    deferGeneratorClose = true;
                }
            }
        }

        for (auto& future : toCancel) future->cancel();

        // Java close() :795 -> generator.close(). With interruptRunning this aborts the running
        // work (its callbacks fire with Cancelled, freeing the slots synchronously).
        if (closeGeneratorNow) generator.close();
    }
};

//==========//
// GenQueue //
//==========//

GenQueue::GenQueue(
    IWorldGenerator& generator,
    IExecutor&       queueThread,
    IExecutor&       genPool,
    int              numThreads,
    IClock&          clock
)
: impl_(std::make_shared<Impl>(generator, queueThread, genPool, numThreads, clock)) {}

GenQueue::~GenQueue() { impl_->shutdown(true); }

std::shared_ptr<RetrievalFuture> GenQueue::submit(std::uint64_t sectionPos, std::uint8_t requiredGenDetail) {
    return impl_->submitRetrievalTask(sectionPos, requiredGenDetail);
}

void GenQueue::startAndSetTargetPos(BlockPos2D target) { impl_->startAndSetTargetPos(target); }

void GenQueue::removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) {
    if (!removeIf) return;
    impl_->removeRetrievalRequestIf(removeIf);
}

std::size_t GenQueue::waitingTaskCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->waitingTaskByPos.size();
}

std::size_t GenQueue::inProgressTaskCount() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->inProgressGenTasksByLodPos.size();
}

void GenQueue::shutdown(bool interruptRunning) { impl_->shutdown(interruptRunning); }

void GenQueue::setNumThreads(int numThreads) {
    std::lock_guard lock(impl_->mutex);
    impl_->numThreads = numThreads;
}

int GenQueue::numThreads() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->numThreads;
}

void GenQueue::setGenerationMode(GenerationMode mode) {
    std::lock_guard lock(impl_->mutex);
    impl_->generatorMode = mode;
}

GenerationMode GenQueue::generationMode() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->generatorMode;
}

std::int8_t GenQueue::lowestDataDetail() const { return impl_->generator.largestDataDetailLevel(); }
std::int8_t GenQueue::highestDataDetail() const { return impl_->generator.smallestDataDetailLevel(); }

// Java :665-675
std::int32_t GenQueue::queuedChunkCount() const {
    std::lock_guard lock(impl_->mutex);
    std::int32_t    chunkCount = 0;
    for (const auto& [pos, task] : impl_->waitingTaskByPos) {
        const std::int32_t chunkWidth = jdiv(sec::blockWidth(pos), lod::CHUNK_WIDTH);
        chunkCount                    = wadd(chunkCount, wmul(chunkWidth, chunkWidth));
    }
    return chunkCount;
}

double GenQueue::averageChunkGenTimeMs() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->rollingAverageChunkGenTimeInMs.average();
}

bool GenQueue::isClosing() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->closing;
}

bool GenQueue::isGeneratorBusy() const { return impl_->isGeneratorBusy(); }

// Java :191-205
bool GenQueue::requestPosExistsWhere(const std::function<bool(std::uint64_t)>& pred) const {
    if (!pred) return false;
    std::vector<std::uint64_t> keys;
    {
        std::lock_guard lock(impl_->mutex);
        keys.reserve(impl_->waitingTaskByPos.size());
        for (const auto& [pos, task] : impl_->waitingTaskByPos) keys.push_back(pos);
    }
    for (std::uint64_t pos : keys) {
        if (pred(pos)) return true;
    }
    return false;
}

std::int32_t GenQueue::taskDistance(std::uint64_t sectionPos, BlockPos2D target) {
    return Impl::taskDistance(sectionPos, target);
}

} // namespace dh
