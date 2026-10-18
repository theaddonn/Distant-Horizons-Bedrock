#pragma once

// Port of util/delayedSaveCache/{AbstractDelayedSaveCache,AbstractSaveObjContainer}.
// Batches multiple updates for the same position and hands the merged container
// to a save handler once it has been idle for `saveDelayMs` (or on flush()).
//
// Differences from Java:
//   * Time comes from the IClock each container is constructed with (nowMillis),
//     so tests use ManualClock.
//   * Virtual methods became constructor callbacks (createContainer / onRemoval).
//   * Containers are shared_ptr: a container returned by write..() stays valid
//     even if cleanUp() removes it concurrently, so Java's "sleep 100 ms to avoid
//     use-after-removal" hack is not needed.
//   * Java had one static daemon thread polling every cache each second. Here
//     cleanUp(false) is public and startBackgroundCleanup() starts an optional
//     per-cache jthread that calls it every `interval` of real time.
//   * The concrete Java caches (DelayedDataSourceSaveCache, DelayedBeaconSaveCache)
//     depend on FullDataSourceV2 / BeaconBeamDTO and are built on this template by
//     the packages that own those types.

#include "dh/platform/IClock.h"
#include "dh/thread/KeyedLock.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dh {

/// Tracks when a payload was last written so the cache can flush it once idle.
template <class TSaveObj>
class SaveObjContainer {
public:
    explicit SaveObjContainer(const IClock& clock) : clock_(&clock), lastWrittenMs_(clock.nowMillis()) {}
    virtual ~SaveObjContainer() = default;

    /// Merge `newObj` (may be null if the concrete cache allows it) into this container.
    virtual void update(const TSaveObj* newObj) = 0;

    void updateLastWrittenTimestamp() { lastWrittenMs_ = clock_->nowMillis(); }

    bool hasTimedOut(std::int64_t msTillTimeout) const {
        return (clock_->nowMillis() - lastWrittenMs_) > msTillTimeout;
    }

    std::int64_t lastWrittenMs() const { return lastWrittenMs_; }

private:
    const IClock* clock_;
    std::int64_t  lastWrittenMs_;
};

template <class TSaveObj, class TContainer>
class DelayedSaveCache {
public:
    using ContainerPtr = std::shared_ptr<TContainer>;
    /// Creates the empty container for a position.
    using CreateFn = std::function<ContainerPtr(std::int64_t pos)>;
    /// Called after the container left the cache (idle timeout or flush).
    using RemovalFn = std::function<void(ContainerPtr)>;

    DelayedSaveCache(CreateFn create, RemovalFn onRemoval, int saveDelayMs)
    : create_(std::move(create)),
      onRemoval_(std::move(onRemoval)),
      saveDelayMs_(saveDelayMs) {}

    ~DelayedSaveCache() { close(); }

    DelayedSaveCache(const DelayedSaveCache&)            = delete;
    DelayedSaveCache& operator=(const DelayedSaveCache&) = delete;

    /// Writes into memory synchronously (so `inputObj` can be released after this
    /// returns) and returns the new or pre-existing container.
    ContainerPtr writeToMemoryAndQueueSave(std::int64_t pos, const TSaveObj* inputObj) {
        auto guard = locks_.lock(pos);

        ContainerPtr container = find(pos);
        if (!container) {
            container = create_(pos);
            ContainerPtr old;
            {
                std::lock_guard lock(mapMutex_);
                auto            it = map_.find(pos);
                if (it != map_.end()) old = std::exchange(it->second, container);
                else map_.emplace(pos, container);
            }
            // shouldn't happen (we hold the key lock), mirrors Java's defensive path
            if (old) onRemoval_(std::move(old));
        }

        container->update(inputObj);
        container->updateLastWrittenTimestamp();
        return container;
    }

    /// Runs `fn()` while holding the position's lock (Java: getLockForPos + lock).
    template <class Fn>
    void withPosLocked(std::int64_t pos, Fn&& fn) {
        auto guard = locks_.lock(pos);
        fn();
    }

    std::size_t unsavedCount() const {
        std::lock_guard lock(mapMutex_);
        return map_.size();
    }

    void flush() { cleanUp(true); }

    /// Removes timed-out containers (or everything if `flushAll`) and fires onRemoval for each.
    void cleanUp(bool flushAll) {
        std::vector<std::int64_t> keys;
        {
            std::lock_guard lock(mapMutex_);
            keys.reserve(map_.size());
            for (auto& kv : map_) keys.push_back(kv.first);
        }

        std::vector<ContainerPtr> removed;
        for (std::int64_t pos : keys) {
            auto         guard = locks_.lock(pos);
            ContainerPtr c     = find(pos);
            if (c && (flushAll || c->hasTimedOut(saveDelayMs_))) {
                {
                    std::lock_guard lock(mapMutex_);
                    map_.erase(pos);
                }
                removed.push_back(std::move(c));
            }
        }

        for (auto& c : removed) onRemoval_(std::move(c));
    }

    /// Optional real-time cleanup thread (Java: static cleanup loop, 1000 ms).
    void startBackgroundCleanup(std::chrono::milliseconds interval = std::chrono::milliseconds(1000)) {
        std::lock_guard lock(threadMutex_);
        if (thread_.joinable()) return;
        thread_ = std::jthread([this, interval](std::stop_token st) {
            std::unique_lock lk(wakeMutex_);
            while (!st.stop_requested()) {
                if (wakeCv_.wait_for(lk, st, interval, [] { return false; })) continue;
                if (st.stop_requested()) break;
                lk.unlock();
                cleanUp(false);
                lk.lock();
            }
        });
    }

    /// Stops the background thread. Does not flush (Java's close() only deregistered).
    void close() {
        std::lock_guard lock(threadMutex_);
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }

    int saveDelayMs() const { return saveDelayMs_; }

private:
    ContainerPtr find(std::int64_t pos) const {
        std::lock_guard lock(mapMutex_);
        auto            it = map_.find(pos);
        return it == map_.end() ? nullptr : it->second;
    }

    CreateFn  create_;
    RemovalFn onRemoval_;
    int       saveDelayMs_;

    mutable std::mutex                             mapMutex_;
    std::unordered_map<std::int64_t, ContainerPtr> map_;
    KeyedLock<std::int64_t>                        locks_;

    std::mutex                  threadMutex_;
    std::mutex                  wakeMutex_;
    std::condition_variable_any wakeCv_;
    std::jthread                thread_;
};

} // namespace dh
