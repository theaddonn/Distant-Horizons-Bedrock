#pragma once

// Deterministic IExecutor for tests: tasks are queued FIFO and run only when the
// test calls runOne()/runAll(). Thread-safe so producers may enqueue from other
// threads, but the run* methods are meant to be driven from one test thread.

#include "dh/platform/IExecutor.h"

#include <deque>
#include <mutex>
#include <utility>

namespace dh {

class ManualExecutor final : public IExecutor {
public:
    bool execute(Task task) override {
        std::lock_guard lock(mutex_);
        if (shutdown_) return false;
        queue_.push_back(std::move(task));
        return true;
    }

    std::size_t queueSize() const override {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    /// Runs the oldest queued task. Returns false if the queue was empty.
    bool runOne() {
        Task task;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) return false;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (task) task();
        return true;
    }

    /// Runs tasks until the queue is empty, including tasks enqueued while
    /// running. Returns the number of tasks run. `maxTasks` bounds runaway loops.
    std::size_t runAll(std::size_t maxTasks = 1'000'000) {
        std::size_t count = 0;
        while (count < maxTasks && runOne()) ++count;
        return count;
    }

    /// Runs only the tasks that were queued when the call started (one "tick").
    std::size_t runPending() {
        std::size_t n     = queueSize();
        std::size_t count = 0;
        while (count < n && runOne()) ++count;
        return count;
    }

    /// After shutdown execute() returns false; queued tasks stay runnable so a
    /// test can still drain them (or clear()).
    void shutdown() {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
    }
    bool isShutdown() const {
        std::lock_guard lock(mutex_);
        return shutdown_;
    }

    /// Discards queued tasks without running them.
    void clear() {
        std::lock_guard lock(mutex_);
        queue_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Task>   queue_;
    bool               shutdown_ = false;
};

} // namespace dh
