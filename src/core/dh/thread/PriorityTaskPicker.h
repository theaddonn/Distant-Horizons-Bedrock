#pragma once

// Port of util/threading/{PriorityTaskPicker,RateLimitedThreadPoolExecutor}.
//
// One fixed pool of `threadCount` worker threads is shared by every Executor
// created from the picker (Java: each executor owned a pool but the picker never
// let more than Config numberOfThreads tasks run at once, i.e. the same budget).
//
// When a worker is free it picks, among executors with queued work whose canRun()
// is true, the one with the highest priority, ties broken by the least total run
// time (Java getExecutorIteratorSortedByShortestTotalRunTime); inside an executor
// tasks are FIFO. Executors whose canRun() is false are skipped, and while any
// such executor has queued work idle workers re-check every 1000 ms (Java
// MS_TO_CHECK_ON_PAUSED_THREADS) or immediately after poke().
//
// runTimeRatio (Java threadRunTimeRatio, RateLimitedThreadPoolExecutor.afterExecute):
// after each task the worker sleeps  runTime / ratio - runTime  so that it is busy
// for `ratio` of the time. Ratios >= 1 (or <= 0, which Java would turn into an
// infinite sleep) disable the sleep. The sleep is interruptible by shutdownNow().
//
// canRun() is invoked while the picker lock is held: it must be quick and must not
// call execute()/clearQueue() on any executor of this picker (queueSize() is fine).
//
// shutdownNow(): further execute() calls return false, queued tasks are dropped
// (Java: they were cleared on the first RejectedExecutionException), running tasks
// finish. joinAll() blocks until all workers exited. The destructor does both.

#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/thread/RollingAverage.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dh {

class PriorityTaskPicker;

class Executor final : public IExecutor {
public:
    bool        execute(Task task) override;
    std::size_t queueSize() const override { return queued_.load(std::memory_order_relaxed); }

    /// Drops all queued (not running) tasks.
    void clearQueue();

    /// Number of tasks currently running.
    int running() const { return running_.load(std::memory_order_relaxed); }
    /// Tasks finished since creation.
    std::int64_t completed() const { return completed_.load(std::memory_order_relaxed); }
    /// NaN if nothing ran yet (see RollingAverage parity note).
    double averageRunTimeMs() const { return runTimeMs_.average(); }

    std::string_view name() const { return name_; }
    int              priority() const { return priority_; }
    bool             canRun() const;
    bool             isShutdown() const;
    /// Worker count of the owning picker (Java getPoolSize).
    int poolSize() const;

private:
    friend class PriorityTaskPicker;
    Executor(PriorityTaskPicker& owner, std::string name, int priority, std::function<bool()> canRun)
    : owner_(owner),
      name_(std::move(name)),
      priority_(priority),
      canRun_(std::move(canRun)) {}

    PriorityTaskPicker&   owner_;
    std::string           name_;
    int                   priority_;
    std::function<bool()> canRun_;

    std::deque<Task>          queue_; // guarded by owner_.mutex_
    std::atomic<std::size_t>  queued_{0};
    std::atomic<int>          running_{0};
    std::atomic<std::int64_t> completed_{0};
    std::int64_t              totalRuntimeNanos_ = 0; // guarded by owner_.mutex_
    RollingAverage            runTimeMs_{200};
};

class PriorityTaskPicker {
public:
    explicit PriorityTaskPicker(int threadCount, float runTimeRatio = 1.0f, const IClock& clock = defaultClock());
    ~PriorityTaskPicker();

    PriorityTaskPicker(const PriorityTaskPicker&)            = delete;
    PriorityTaskPicker& operator=(const PriorityTaskPicker&) = delete;

    /// Higher `priority` runs first (Java Thread.NORM_PRIORITY = 5). `canRun` empty = always.
    /// The returned reference is valid for the picker's lifetime.
    Executor& createExecutor(std::string name, int priority = kNormPriority, std::function<bool()> canRun = {});

    /// Re-evaluates canRun() predicates now instead of at the next 1 s poll.
    void poke();

    /// Rejects new tasks, drops queued ones, wakes sleeping workers. Does not block.
    void shutdownNow();
    /// Blocks until every worker thread has exited (call after shutdownNow()).
    void joinAll();

    bool isShutdown() const { return shutdown_.load(std::memory_order_acquire); }
    int  threadCount() const { return threadCount_; }

    static constexpr int kNormPriority = 5;

    static const IClock& defaultClock() {
        static const SystemClock clock;
        return clock;
    }

private:
    friend class Executor;

    bool enqueue(Executor& ex, IExecutor::Task task);
    void clear(Executor& ex);
    void workerLoop();
    // Requires mutex_. Returns the executor to run from, or null; sets `paused`
    // if a non-empty executor was skipped because canRun() was false.
    Executor* pickLocked(bool& paused);

    const IClock& clock_;
    const int     threadCount_;
    const double  ratio_;

    mutable std::mutex                     mutex_;
    std::condition_variable                workCv_;
    std::condition_variable                sleepCv_;
    std::vector<std::unique_ptr<Executor>> executors_;
    std::atomic<bool>                      shutdown_{false};

    std::mutex               joinMutex_;
    std::vector<std::thread> workers_;
};

} // namespace dh
