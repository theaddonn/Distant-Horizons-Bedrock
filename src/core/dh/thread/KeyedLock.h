#pragma once

// Per-key mutual exclusion (Java: util/KeyedLockContainer + threading/PositionalLockProvider).
//
// Java mapped an unbounded key space onto a fixed array of ReentrantLocks
// (hash collisions = false sharing) and, in PositionalLockProvider, onto a map of
// locks that a cleanup thread expired after 10 s. Here every live key gets its
// own mutex in a table; an entry exists only while some thread holds or waits for
// it (reference counted), so there are no collisions and no cleanup thread.
//
// Differences from Java: the locks are NOT reentrant (locking a key you already
// hold deadlocks), and lock()/tryLock() return a move-only Guard rather than
// std::unique_lock<std::mutex> (a bare unique_lock cannot drop the table
// reference on release). Guard offers owns_lock()/unlock()/explicit bool.

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace dh {

template <class K, class Hash = std::hash<K>>
class KeyedLock {
    struct Entry {
        std::mutex mutex;
        int        refs = 0; // guarded by KeyedLock::tableMutex_
    };

public:
    class Guard {
    public:
        Guard() = default;
        Guard(Guard&& o) noexcept
        : owner_(std::exchange(o.owner_, nullptr)),
          entry_(o.entry_),
          key_(std::move(o.key_)) {}
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                unlock();
                owner_ = std::exchange(o.owner_, nullptr);
                entry_ = o.entry_;
                key_   = std::move(o.key_);
            }
            return *this;
        }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { unlock(); }

        bool     owns_lock() const { return owner_ != nullptr; }
        explicit operator bool() const { return owns_lock(); }

        void unlock() {
            if (owner_ != nullptr) {
                KeyedLock* owner = std::exchange(owner_, nullptr);
                owner->release(key_, *entry_);
            }
        }

        /// Identity of the underlying per-key mutex while held.
        const std::mutex* mutex() const { return owner_ != nullptr ? &entry_->mutex : nullptr; }

    private:
        friend class KeyedLock;
        Guard(KeyedLock* owner, Entry* entry, K key) : owner_(owner), entry_(entry), key_(std::move(key)) {}

        KeyedLock* owner_ = nullptr;
        Entry*     entry_ = nullptr;
        K          key_{};
    };

    /// Blocks until the key's lock is held.
    [[nodiscard]] Guard lock(const K& key) {
        Entry* entry = acquireEntry(key);
        entry->mutex.lock();
        return Guard(this, entry, key);
    }

    /// Returns nullopt if another thread holds the key.
    [[nodiscard]] std::optional<Guard> tryLock(const K& key) {
        Entry* entry = acquireEntry(key);
        if (!entry->mutex.try_lock()) {
            dropRef(key, *entry);
            return std::nullopt;
        }
        return Guard(this, entry, key);
    }

    /// Number of keys currently held or waited on (0 when idle).
    std::size_t liveKeyCount() const {
        std::lock_guard lock(tableMutex_);
        return table_.size();
    }

private:
    Entry* acquireEntry(const K& key) {
        std::lock_guard lock(tableMutex_);
        Entry&          e = table_[key];
        ++e.refs;
        return &e; // unordered_map nodes are address-stable, and refs > 0 keeps it alive
    }

    void dropRef(const K& key, Entry& entry) {
        std::lock_guard lock(tableMutex_);
        if (--entry.refs == 0) table_.erase(key);
    }

    void release(const K& key, Entry& entry) {
        entry.mutex.unlock();
        dropRef(key, entry);
    }

    mutable std::mutex                 tableMutex_;
    std::unordered_map<K, Entry, Hash> table_;
};

} // namespace dh
