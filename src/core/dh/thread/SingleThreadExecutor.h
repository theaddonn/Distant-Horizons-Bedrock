#pragma once

// Java ThreadUtil.makeSingleThreadPool / makeSingleDaemonThreadPool: one worker
// thread draining a FIFO queue, independent of the PriorityTaskPicker budget.
//
//   shutdown()     reject new tasks; queued tasks still run (ThreadPoolExecutor.shutdown)
//   shutdownNow()  reject new tasks; drop queued tasks      (ThreadPoolExecutor.shutdownNow)
//   join()         block until the worker exited; the destructor does shutdown() + join()

#include "dh/platform/IExecutor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace dh {

class SingleThreadExecutor final : public IExecutor {
public:
    explicit SingleThreadExecutor(std::string name) : name_(std::move(name)), worker_([this] { loop(); }) {}

    ~SingleThreadExecutor() {
        shutdown();
        join();
    }

    SingleThreadExecutor(const SingleThreadExecutor&)            = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;

    bool execute(Task task) override {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return false;
            queue_.push_back(std::move(task));
            queued_.store(queue_.size(), std::memory_order_relaxed);
        }
        cv_.notify_one();
        return true;
    }

    std::size_t queueSize() const override { return queued_.load(std::memory_order_relaxed); }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    void shutdownNow() {
        std::deque<Task> dropped;
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
            dropped.swap(queue_);
            queued_.store(0, std::memory_order_relaxed);
        }
        cv_.notify_all();
    }

    void join() {
        std::lock_guard lock(joinMutex_);
        if (worker_.joinable()) worker_.join();
    }

    bool isShutdown() const {
        std::lock_guard lock(mutex_);
        return shutdown_;
    }

    std::string_view name() const { return name_; }

private:
    void loop() {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
            if (queue_.empty()) return; // shutdown and drained
            Task task = std::move(queue_.front());
            queue_.pop_front();
            queued_.store(queue_.size(), std::memory_order_relaxed);
            lock.unlock();
            if (task) task();
            task = nullptr;
            lock.lock();
        }
    }

    std::string              name_;
    mutable std::mutex       mutex_;
    std::condition_variable  cv_;
    std::deque<Task>         queue_;
    std::atomic<std::size_t> queued_{0};
    bool                     shutdown_ = false;
    std::mutex               joinMutex_;
    std::thread              worker_; // declared last: starts after the other members exist
};

} // namespace dh
