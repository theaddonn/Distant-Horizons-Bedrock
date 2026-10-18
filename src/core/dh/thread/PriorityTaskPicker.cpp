#include "dh/thread/PriorityTaskPicker.h"

#include <algorithm>
#include <chrono>

namespace dh {

namespace {
constexpr std::chrono::milliseconds kPausedRecheck{1000}; // Java MS_TO_CHECK_ON_PAUSED_THREADS
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

bool Executor::execute(Task task) { return owner_.enqueue(*this, std::move(task)); }

void Executor::clearQueue() { owner_.clear(*this); }

bool Executor::canRun() const { return !canRun_ || canRun_(); }

bool Executor::isShutdown() const { return owner_.isShutdown(); }

int Executor::poolSize() const { return owner_.threadCount(); }

// ---------------------------------------------------------------------------
// PriorityTaskPicker
// ---------------------------------------------------------------------------

PriorityTaskPicker::PriorityTaskPicker(int threadCount, float runTimeRatio, const IClock& clock)
: clock_(clock),
  threadCount_(std::max(1, threadCount)),
  ratio_(static_cast<double>(runTimeRatio)) {
    workers_.reserve(static_cast<std::size_t>(threadCount_));
    for (int i = 0; i < threadCount_; ++i) workers_.emplace_back([this] { workerLoop(); });
}

PriorityTaskPicker::~PriorityTaskPicker() {
    shutdownNow();
    joinAll();
}

Executor& PriorityTaskPicker::createExecutor(std::string name, int priority, std::function<bool()> canRun) {
    std::unique_ptr<Executor> ex(new Executor(*this, std::move(name), priority, std::move(canRun)));
    Executor&                 ref = *ex;
    std::lock_guard           lock(mutex_);
    executors_.push_back(std::move(ex));
    return ref;
}

void PriorityTaskPicker::poke() {
    { std::lock_guard lock(mutex_); }
    workCv_.notify_all();
}

bool PriorityTaskPicker::enqueue(Executor& ex, IExecutor::Task task) {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_.load(std::memory_order_relaxed)) return false;
        ex.queue_.push_back(std::move(task));
        ex.queued_.store(ex.queue_.size(), std::memory_order_relaxed);
    }
    workCv_.notify_one();
    return true;
}

void PriorityTaskPicker::clear(Executor& ex) {
    std::deque<IExecutor::Task> dropped;
    {
        std::lock_guard lock(mutex_);
        dropped.swap(ex.queue_);
        ex.queued_.store(0, std::memory_order_relaxed);
    }
    // `dropped` destroyed outside the lock (captured state may be heavy or re-enter)
}

void PriorityTaskPicker::shutdownNow() {
    std::vector<std::deque<IExecutor::Task>> dropped;
    {
        std::lock_guard lock(mutex_);
        shutdown_.store(true, std::memory_order_release);
        for (auto& ex : executors_) {
            dropped.emplace_back();
            dropped.back().swap(ex->queue_);
            ex->queued_.store(0, std::memory_order_relaxed);
        }
    }
    workCv_.notify_all();
    sleepCv_.notify_all();
}

void PriorityTaskPicker::joinAll() {
    std::lock_guard lock(joinMutex_);
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

Executor* PriorityTaskPicker::pickLocked(bool& paused) {
    Executor* best = nullptr;
    for (auto& up : executors_) {
        Executor& ex = *up;
        if (ex.queue_.empty()) continue;
        if (!ex.canRun()) {
            paused = true;
            continue;
        }
        if (best == nullptr || ex.priority_ > best->priority_
            || (ex.priority_ == best->priority_ && ex.totalRuntimeNanos_ < best->totalRuntimeNanos_)) {
            best = &ex;
        }
    }
    return best;
}

void PriorityTaskPicker::workerLoop() {
    std::unique_lock lock(mutex_);
    while (true) {
        if (shutdown_.load(std::memory_order_relaxed)) return;

        bool      paused = false;
        Executor* ex     = pickLocked(paused);
        if (ex == nullptr) {
            if (paused) workCv_.wait_for(lock, kPausedRecheck);
            else workCv_.wait(lock);
            continue;
        }

        IExecutor::Task task = std::move(ex->queue_.front());
        ex->queue_.pop_front();
        ex->queued_.store(ex->queue_.size(), std::memory_order_relaxed);
        ex->running_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();

        std::int64_t start = clock_.steadyNanos();
        if (task) task();
        task               = nullptr; // release captures before accounting
        std::int64_t taken = std::max<std::int64_t>(0, clock_.steadyNanos() - start);

        ex->runTimeMs_.add(static_cast<double>(taken / 1'000'000));
        ex->completed_.fetch_add(1, std::memory_order_relaxed);
        ex->running_.fetch_sub(1, std::memory_order_relaxed);

        lock.lock();
        ex->totalRuntimeNanos_ += taken;

        // threadRunTimeRatio throttle: sleep (runTime / ratio - runTime)
        if (ratio_ > 0.0 && ratio_ < 1.0 && taken > 0) {
            auto sleepNanos =
                static_cast<std::int64_t>(static_cast<double>(taken) / ratio_ - static_cast<double>(taken));
            sleepCv_.wait_for(lock, std::chrono::nanoseconds(sleepNanos), [this] {
                return shutdown_.load(std::memory_order_relaxed);
            });
        }
    }
}

} // namespace dh
