// WP-H: thread pools, priority picker, keyed locks, rate limiters, delayed save cache.
//
// ThreadSanitizer (the default test build uses ASan/UBSan, which cannot be combined with TSan):
//   cd tests && clang++-22 -std=c++20 -g -O1 -fsanitize=thread -I. -Ithird_party \
//     -I../src/core -I../src/core/third_party main.cpp unit/thread_test.cpp \
//     ../src/core/dh/thread/PriorityTaskPicker.cpp ../src/core/dh/thread/ThreadPools.cpp \
//     -o ~/.cache/dh-tsan/thread_tsan -pthread && ~/.cache/dh-tsan/thread_tsan -tc='thread:*'
// Last run: 23/23 passed, zero TSan reports.
//
// Waiting in tests is always a bounded condition-variable wait, never a sleep.

#include <doctest.h>

#include "dh/platform/IClock.h"
#include "dh/thread/DelayedSaveCache.h"
#include "dh/thread/KeyedLock.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/thread/PriorityTaskPicker.h"
#include "dh/thread/RateLimiter.h"
#include "dh/thread/RollingAverage.h"
#include "dh/thread/SingleThreadExecutor.h"
#include "dh/thread/ThreadPools.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dh;

namespace {

constexpr auto kWait = std::chrono::seconds(20);

/// One-shot gate: workers block in wait() until open().
class Gate {
public:
    void open() {
        // notify under the lock: a waiter may destroy the Gate right after it wakes
        std::lock_guard lock(m_);
        open_ = true;
        cv_.notify_all();
    }
    bool wait() {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, kWait, [this] { return open_; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

/// Counter with a bounded wait for "reached n".
class Counter {
public:
    void add(int n = 1) {
        // notify under the lock: a waiter may destroy the Counter right after it wakes
        std::lock_guard lock(m_);
        value_ += n;
        cv_.notify_all();
    }
    int get() {
        std::lock_guard lock(m_);
        return value_;
    }
    bool waitFor(int n) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, kWait, [&] { return value_ >= n; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    int                     value_ = 0;
};

/// Thread-safe clock for throttle tests.
class AtomicClock final : public IClock {
public:
    std::int64_t nowMillis() const override { return nanos_.load() / 1'000'000; }
    std::int64_t steadyNanos() const override { return nanos_.load(); }
    void         advanceNanos(std::int64_t d) { nanos_.fetch_add(d); }

private:
    std::atomic<std::int64_t> nanos_{0};
};

struct Order {
    std::mutex       m;
    std::vector<int> ids;
    void             push(int id) {
        std::lock_guard lock(m);
        ids.push_back(id);
    }
    std::vector<int> snapshot() {
        std::lock_guard lock(m);
        return ids;
    }
};

/// Occupies the picker's only worker until `release` opens. Returns once it is running.
void blockSingleWorker(Executor& ex, Gate& release) {
    Counter started;
    REQUIRE(ex.execute([&started, &release] {
        started.add();
        release.wait();
    }));
    REQUIRE(started.waitFor(1));
}

} // namespace

// ---------------------------------------------------------------------------
// KeyedLockTest.java (BasicKeyedLockTest) + contention
// ---------------------------------------------------------------------------

TEST_CASE("thread: KeyedLock same key maps to the same lock (KeyedLockTest)") {
    KeyedLock<std::int64_t> container;

    for (std::int64_t a = -10; a < 10; a++) {
        auto aLock = container.lock(a);
        REQUIRE(aLock.owns_lock());
        for (std::int64_t b = -10; b < 10; b++) {
            auto bLock = container.tryLock(b);
            if (a == b) {
                // same position always maps to the same lock: it is already held
                CHECK_FALSE(bLock.has_value());
            } else {
                // per-key table: different keys never collide (stricter than Java's hashed array)
                CHECK(bLock.has_value());
            }
        }
    }
    CHECK(container.liveKeyCount() == 0);
}

TEST_CASE("thread: KeyedLock guard lifecycle and reference counting") {
    KeyedLock<int> kl;
    {
        auto g = kl.lock(7);
        CHECK(kl.liveKeyCount() == 1);
        CHECK(g.mutex() != nullptr);
        auto moved = std::move(g);
        CHECK_FALSE(g.owns_lock());
        CHECK(moved.owns_lock());
        CHECK_FALSE(kl.tryLock(7).has_value());
        CHECK(kl.liveKeyCount() == 1); // failed tryLock must not leak its reference
        moved.unlock();
        CHECK_FALSE(moved.owns_lock());
        CHECK(kl.liveKeyCount() == 0);
    }
    auto again = kl.tryLock(7);
    CHECK(again.has_value());
}

TEST_CASE("thread: KeyedLock tryLock contention across threads") {
    KeyedLock<int> kl;
    constexpr int  kThreads = 4;
    constexpr int  kIters   = 5000;

    int              guarded = 0; // protected by key 1; a data race here would trip TSan/ASan
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                if (i % 2 == 0) {
                    auto g = kl.lock(1);
                    ++guarded;
                    successes.fetch_add(1);
                } else if (auto g = kl.tryLock(1)) {
                    ++guarded;
                    successes.fetch_add(1);
                } else {
                    failures.fetch_add(1);
                }
                auto other = kl.lock(100 + t); // distinct keys never contend
                (void)other;
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(guarded == successes.load());
    CHECK(successes.load() + failures.load() == kThreads * kIters);
    CHECK(kl.liveKeyCount() == 0);
}

TEST_CASE("thread: KeyedLock blocks a second locker until release") {
    KeyedLock<int> kl;
    auto           g = kl.lock(5);

    Counter           entered;
    std::atomic<bool> holderReleased{false};
    std::atomic<bool> sawEarly{false};
    std::thread       t([&] {
        auto g2 = kl.lock(5);
        if (!holderReleased.load()) sawEarly = true;
        entered.add();
    });
    // t is (about to be) blocked; releasing must let it through
    holderReleased = true;
    g.unlock();
    REQUIRE(entered.waitFor(1));
    t.join();
    CHECK_FALSE(sawEarly.load());
}

// ---------------------------------------------------------------------------
// RollingAverage
// ---------------------------------------------------------------------------

TEST_CASE("thread: RollingAverage matches Java (including its divisor quirk)") {
    RollingAverage avg(4);
    CHECK(std::isnan(avg.average()));

    avg.add(10.0); // Java: currentSize = max(index+1 = 2, 0) -> 10/2
    CHECK(avg.average() == doctest::Approx(5.0));
    avg.add(10.0);
    avg.add(10.0);
    avg.add(10.0); // ring full, divisor 4
    CHECK(avg.average() == doctest::Approx(10.0));
    avg.add(2.0); // overwrites oldest
    CHECK(avg.average() == doctest::Approx(8.0));
    CHECK(avg.lifetimeCount() == 5);

    avg.clear();
    CHECK(avg.lifetimeCount() == 0);
    CHECK(std::isnan(avg.average()));
}

// ---------------------------------------------------------------------------
// Rate limiters (ManualClock)
// ---------------------------------------------------------------------------

TEST_CASE("thread: SupplierBasedRateLimiter follows Guava SmoothBursty") {
    ManualClock clock;
    int         rate     = 10; // permits per second -> 100 ms apart
    int         failures = 0;

    SupplierBasedRateLimiter<int> limiter([&] { return rate; }, [&](const int&) { ++failures; }, clock);

    // Guava is created with an infinite rate; the first setRate(10) hands out a full second of burst
    int got = 0;
    while (limiter.tryAcquire()) ++got;
    CHECK(got == 11); // 10 banked permits + the free next slot
    CHECK(failures == 1);

    // now permits trickle in every 100 ms
    clock.advanceMillis(99);
    CHECK_FALSE(limiter.tryAcquire());
    clock.advanceMillis(1);
    CHECK(limiter.tryAcquire());
    CHECK_FALSE(limiter.tryAcquire());

    // idle for a long time: the burst is banked again
    clock.advanceMillis(60'000);
    got = 0;
    while (limiter.tryAcquire()) ++got;
    CHECK(got == 11);

    // acquireAll() blocks everything for about a second
    clock.advanceMillis(60'000);
    limiter.acquireAll();
    CHECK_FALSE(limiter.tryAcquire());
    clock.advanceMillis(1'000);
    CHECK(limiter.tryAcquire());

    // a supplier of 0 never grants
    rate = 0;
    clock.advanceMillis(10'000);
    CHECK_FALSE(limiter.tryAcquire());
}

TEST_CASE("thread: SupplierBasedRateLimiter acquireOrDrain") {
    ManualClock                   clock;
    SupplierBasedRateLimiter<int> limiter([] { return 4; }, {}, clock);
    CHECK(limiter.acquireOrDrain(2) == 2);
    CHECK(limiter.acquireOrDrain(100) == 3); // 2 banked + the free slot
    CHECK(limiter.acquireOrDrain(100) == 0);
}

TEST_CASE("thread: SupplierBasedConcurrencyLimiter and combined limiter") {
    int                                  failed = 0;
    SupplierBasedConcurrencyLimiter<int> conc([] { return 2; }, [&](const int&) { ++failed; });
    CHECK(conc.tryAcquire());
    CHECK(conc.tryAcquire());
    CHECK_FALSE(conc.tryAcquire());
    CHECK(failed == 1);
    conc.release();
    CHECK(conc.tryAcquire());

    ManualClock                                 clock;
    SupplierBasedRateAndConcurrencyLimiter<int> both([] { return 1; }, {}, clock);
    CHECK(both.tryAcquire());
    // rate limiter refuses, so the concurrency slot must have been given back
    CHECK_FALSE(both.tryAcquire());
    both.release();
    clock.advanceMillis(1'000);
    CHECK(both.tryAcquire());
}

// ---------------------------------------------------------------------------
// DelayedSaveCacheTest.java (CacheExpirationAndPoolingTest, driven by ManualClock)
// ---------------------------------------------------------------------------

namespace {
struct IntContainer : SaveObjContainer<int> {
    using SaveObjContainer<int>::SaveObjContainer;
    std::int64_t pos     = 0;
    int          sum     = 0;
    int          updates = 0;
    void         update(const int* v) override {
        ++updates;
        if (v != nullptr) sum += *v;
    }
};
} // namespace

TEST_CASE("thread: DelayedSaveCache expiration (DelayedSaveCacheTest)") {
    ManualClock clock;
    int         diskSaves  = 0;
    int         savedTotal = 0;

    DelayedSaveCache<int, IntContainer> cache(
        [&](std::int64_t pos) {
            auto c = std::make_shared<IntContainer>(clock);
            c->pos = pos;
            return c;
        },
        [&](std::shared_ptr<IntContainer> c) {
            ++diskSaves;
            savedTotal += c->sum;
        },
        1'000
    );

    // single item and manual flush
    int one = 1;
    cache.writeToMemoryAndQueueSave(0, &one);
    CHECK_MESSAGE(cache.unsavedCount() == 1, "only 1 item should be in the cache");
    CHECK_MESSAGE(diskSaves == 0, "no disk saves should have happened yet");
    cache.flush();
    CHECK_MESSAGE(cache.unsavedCount() == 0, "memory cache should be empty after");
    CHECK_MESSAGE(diskSaves == 1, "1 manual flush was expected");

    // quick group: multiple writes for the same position merge into one container
    std::shared_ptr<IntContainer> last;
    for (int i = 0; i < 4; i++) last = cache.writeToMemoryAndQueueSave(0, &one);
    CHECK(cache.unsavedCount() == 1);
    CHECK(last->updates == 4);
    CHECK(diskSaves == 1);

    // not yet timed out (delay is strict >)
    clock.advanceMillis(1'000);
    cache.cleanUp(false);
    CHECK(cache.unsavedCount() == 1);
    clock.advanceMillis(1);
    cache.cleanUp(false);
    CHECK_MESSAGE(cache.unsavedCount() == 0, "cache should clear due to inactivity");
    CHECK_MESSAGE(diskSaves == 2, "second save after timeout expected");
    CHECK(savedTotal == 1 + 4);

    // slow group: each write resets the timer, so nothing is saved until the last write goes idle
    for (int i = 0; i < 4; i++) {
        cache.writeToMemoryAndQueueSave(0, &one);
        clock.advanceMillis(500);
        cache.cleanUp(false);
    }
    CHECK(cache.unsavedCount() == 1);
    CHECK(diskSaves == 2);
    clock.advanceMillis(1'000);
    cache.cleanUp(false);
    CHECK(cache.unsavedCount() == 0);
    CHECK_MESSAGE(diskSaves == 3, "third timeout expected");

    // independent positions time out independently
    cache.writeToMemoryAndQueueSave(1, &one);
    clock.advanceMillis(600);
    cache.writeToMemoryAndQueueSave(2, &one);
    clock.advanceMillis(600);
    cache.cleanUp(false);
    CHECK(cache.unsavedCount() == 1);
    CHECK(diskSaves == 4);
}

TEST_CASE("thread: DelayedSaveCache concurrent writers and background cleanup") {
    SystemClock      clock;
    Counter          removed;
    std::atomic<int> saved{0};

    DelayedSaveCache<int, IntContainer> cache(
        [&](std::int64_t) { return std::make_shared<IntContainer>(clock); },
        [&](std::shared_ptr<IntContainer> c) {
            saved.fetch_add(c->sum);
            removed.add();
        },
        0
    );
    cache.startBackgroundCleanup(std::chrono::milliseconds(5));

    constexpr int            kThreads = 4;
    constexpr int            kWrites  = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            int one = 1;
            for (int i = 0; i < kWrites; ++i) cache.writeToMemoryAndQueueSave(i % 8, &one);
        });
    }
    for (auto& th : threads) th.join();

    cache.close(); // stop the cleaner, then flush the remainder
    cache.flush();
    CHECK(cache.unsavedCount() == 0);
    CHECK(saved.load() == kThreads * kWrites); // no write lost, none saved twice
}

// ---------------------------------------------------------------------------
// PriorityTaskPicker
// ---------------------------------------------------------------------------

TEST_CASE("thread: picker runs tasks by priority, then FIFO") {
    PriorityTaskPicker picker(1);
    Executor&          blocker = picker.createExecutor("blocker", 100);
    Executor&          low     = picker.createExecutor("low", 1);
    Executor&          mid     = picker.createExecutor("mid", 5);
    Executor&          high    = picker.createExecutor("high", 9);

    Gate release;
    blockSingleWorker(blocker, release);

    Order   order;
    Counter done;
    for (int i = 0; i < 3; ++i)
        REQUIRE(low.execute([&, i] {
            order.push(100 + i);
            done.add();
        }));
    for (int i = 0; i < 3; ++i)
        REQUIRE(mid.execute([&, i] {
            order.push(50 + i);
            done.add();
        }));
    for (int i = 0; i < 3; ++i)
        REQUIRE(high.execute([&, i] {
            order.push(i);
            done.add();
        }));

    CHECK(low.queueSize() == 3);
    CHECK(mid.queueSize() == 3);
    CHECK(high.queueSize() == 3);

    release.open();
    REQUIRE(done.waitFor(9));
    CHECK(order.snapshot() == std::vector<int>{0, 1, 2, 50, 51, 52, 100, 101, 102});
    picker.shutdownNow();
    picker.joinAll();
}

TEST_CASE("thread: picker skips paused executors until canRun flips") {
    PriorityTaskPicker picker(1);
    Executor&          blocker = picker.createExecutor("blocker", 100);
    std::atomic<bool>  allowed{false};
    Executor&          paused = picker.createExecutor("paused", 9, [&] { return allowed.load(); });
    Executor&          normal = picker.createExecutor("normal", 1);

    Gate release;
    blockSingleWorker(blocker, release);

    Order   order;
    Counter done;
    for (int i = 0; i < 2; ++i)
        REQUIRE(paused.execute([&, i] {
            order.push(10 + i);
            done.add();
        }));
    for (int i = 0; i < 2; ++i)
        REQUIRE(normal.execute([&, i] {
            order.push(i);
            done.add();
        }));

    release.open();
    // the higher-priority executor is paused, so the low-priority one runs first
    REQUIRE(done.waitFor(2));
    CHECK(order.snapshot() == std::vector<int>{0, 1});
    CHECK(paused.queueSize() == 2);
    CHECK(paused.running() == 0);

    allowed = true;
    picker.poke();
    REQUIRE(done.waitFor(4));
    CHECK(order.snapshot() == std::vector<int>{0, 1, 10, 11});
    CHECK(paused.queueSize() == 0);

    // pausing again holds new work back
    allowed = false;
    Counter extra;
    REQUIRE(paused.execute([&] { extra.add(); }));
    picker.poke();
    CHECK(paused.queueSize() == 1);
    allowed = true;
    picker.poke();
    REQUIRE(extra.waitFor(1));

    picker.shutdownNow();
    picker.joinAll();
}

TEST_CASE("thread: picker rejects after shutdown and never runs rejected or queued tasks") {
    PriorityTaskPicker picker(1);
    Executor&          ex = picker.createExecutor("ex");

    Gate release;
    blockSingleWorker(ex, release);

    std::atomic<int> ran{0};
    for (int i = 0; i < 5; ++i) REQUIRE(ex.execute([&] { ran.fetch_add(1); }));
    CHECK(ex.queueSize() == 5);

    picker.shutdownNow();
    CHECK(picker.isShutdown());
    CHECK(ex.queueSize() == 0); // queued tasks are dropped
    CHECK_FALSE(ex.execute([&] { ran.fetch_add(1); }));
    CHECK(ex.queueSize() == 0);

    release.open();
    picker.joinAll();
    CHECK(ran.load() == 0);
    CHECK(ex.running() == 0);
    CHECK_FALSE(ex.execute([&] { ran.fetch_add(1); }));
    CHECK(ran.load() == 0);
}

TEST_CASE("thread: Executor clearQueue, name, completed") {
    PriorityTaskPicker picker(1);
    Executor&          blocker = picker.createExecutor("blocker", 100);
    Executor&          ex      = picker.createExecutor("named", 3);
    CHECK(ex.name() == "named");
    CHECK(ex.priority() == 3);
    CHECK(ex.poolSize() == 1);

    Gate release;
    blockSingleWorker(blocker, release);
    std::atomic<int> ran{0};
    for (int i = 0; i < 4; ++i) REQUIRE(ex.execute([&] { ran.fetch_add(1); }));
    CHECK(ex.queueSize() == 4);
    ex.clearQueue();
    CHECK(ex.queueSize() == 0);

    Counter done;
    REQUIRE(ex.execute([&] { done.add(); }));
    release.open();
    REQUIRE(done.waitFor(1));
    CHECK(ran.load() == 0);
    CHECK(blocker.running() <= 1);
    picker.shutdownNow();
    picker.joinAll();
}

TEST_CASE("thread: picker never exceeds its thread budget") {
    constexpr int      kThreads = 3;
    PriorityTaskPicker picker(kThreads);
    Executor&          a = picker.createExecutor("a");
    Executor&          b = picker.createExecutor("b", 7);

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    Counter          done;
    constexpr int    kTasks = 2000;
    auto             work   = [&] {
        int now  = active.fetch_add(1) + 1;
        int prev = peak.load();
        while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
        active.fetch_sub(1);
        done.add();
    };
    for (int i = 0; i < kTasks; ++i) {
        REQUIRE(a.execute(work));
        REQUIRE(b.execute(work));
    }
    REQUIRE(done.waitFor(2 * kTasks));
    CHECK(peak.load() <= kThreads);
    CHECK(a.completed() + b.completed() == 2 * kTasks);
    picker.shutdownNow();
    picker.joinAll();
}

TEST_CASE("thread: picker runTimeRatio throttles workers") {
    AtomicClock        clock;
    PriorityTaskPicker picker(1, 0.5f, clock);
    Executor&          ex = picker.createExecutor("ex");

    // each task "runs" 20 ms on the injected clock, so ratio 0.5 sleeps 20 ms of real time after it
    Counter done;
    auto    start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(ex.execute([&] {
            clock.advanceNanos(20'000'000);
            done.add();
        }));
    }
    REQUIRE(done.waitFor(3));
    // the third task finishes after two throttle sleeps (the last sleep happens after it)
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= std::chrono::milliseconds(35));
    picker.shutdownNow(); // must interrupt a throttle sleep promptly
    picker.joinAll();
}

TEST_CASE("thread: picker stress, 1e5 tasks over 5 executors") {
    constexpr int kExecutors   = 5;
    constexpr int kPerExecutor = 20'000;
    constexpr int kTotal       = kExecutors * kPerExecutor;

    PriorityTaskPicker picker(4);
    Executor*          executors[kExecutors];
    for (int i = 0; i < kExecutors; ++i) executors[i] = &picker.createExecutor("ex" + std::to_string(i), i % 3);

    std::atomic<int> rejected{0};
    std::atomic<int> perExecutor[kExecutors];
    for (auto& c : perExecutor) c = 0;
    Counter done;

    // two producers feed all executors concurrently
    std::vector<std::thread> producers;
    for (int p = 0; p < 2; ++p) {
        producers.emplace_back([&, p] {
            for (int i = p; i < kPerExecutor; i += 2) {
                for (int e = 0; e < kExecutors; ++e) {
                    bool accepted = executors[e]->execute([&, e] {
                        perExecutor[e].fetch_add(1);
                        done.add();
                    });
                    if (!accepted) rejected.fetch_add(1); // doctest asserts are not thread-safe
                }
            }
        });
    }
    for (auto& t : producers) t.join();

    REQUIRE(done.waitFor(kTotal));
    // completed() is bumped by the worker AFTER the task body returns (so after done.add()):
    // reading it right after waitFor() races with the last in-flight tasks. Barrier first.
    picker.shutdownNow(); // every task already ran (done == kTotal), nothing is left to drop
    picker.joinAll();
    CHECK(rejected.load() == 0);
    for (int e = 0; e < kExecutors; ++e) {
        CHECK(perExecutor[e].load() == kPerExecutor);
        CHECK(executors[e]->completed() == kPerExecutor);
        CHECK(executors[e]->queueSize() == 0);
        CHECK(executors[e]->running() == 0);
    }
}

TEST_CASE("thread: picker completed() counts a task only after its body returned") {
    // Deterministic form of the stress-test flake: a task that has signalled from inside its body
    // is not yet "completed" until it returns; joinAll() is the barrier that makes it exact.
    PriorityTaskPicker picker(1);
    Executor&          ex = picker.createExecutor("ex");

    Counter                 inBody;
    std::mutex              m;
    std::condition_variable cv;
    bool                    release = false;

    REQUIRE(ex.execute([&] {
        inBody.add();
        std::unique_lock lock(m);
        cv.wait_for(lock, kWait, [&] { return release; });
    }));
    REQUIRE(inBody.waitFor(1));
    CHECK(ex.completed() == 0);
    CHECK(ex.running() == 1);
    {
        std::lock_guard lock(m);
        release = true;
    }
    cv.notify_all();
    picker.shutdownNow();
    picker.joinAll();
    CHECK(ex.completed() == 1);
    CHECK(ex.running() == 0);
}

TEST_CASE("thread: picker shutdown while running") {
    PriorityTaskPicker picker(4);
    Executor*          executors[3];
    for (int i = 0; i < 3; ++i) executors[i] = &picker.createExecutor("ex" + std::to_string(i));

    std::atomic<int> accepted{0};
    std::atomic<int> ran{0};
    Counter          started;

    std::thread producer([&] {
        for (int i = 0; i < 200'000; ++i) {
            if (executors[i % 3]->execute([&] {
                    ran.fetch_add(1);
                    started.add();
                })) {
                accepted.fetch_add(1);
            } else {
                break; // rejected after shutdown
            }
        }
    });

    REQUIRE(started.waitFor(100)); // genuinely running before we pull the plug
    picker.shutdownNow();
    producer.join();
    picker.joinAll();

    for (auto* ex : executors) {
        CHECK(ex->queueSize() == 0);
        CHECK(ex->running() == 0);
        CHECK_FALSE(ex->execute([] {}));
    }
    CHECK(ran.load() <= accepted.load());
    CHECK(ran.load() >= 100);
}

TEST_CASE("thread: picker works alongside the test-double executors") {
    // ManualExecutor / InlineExecutor share IExecutor; code written against it must accept all
    ManualExecutor     manual;
    PriorityTaskPicker picker(1);
    IExecutor&         real = picker.createExecutor("real");

    Counter    done;
    IExecutor* all[] = {&manual, &real};
    for (IExecutor* e : all) CHECK(e->execute([&] { done.add(); }));
    manual.runAll();
    REQUIRE(done.waitFor(2));
    picker.shutdownNow();
    picker.joinAll();
}

// ---------------------------------------------------------------------------
// SingleThreadExecutor
// ---------------------------------------------------------------------------

TEST_CASE("thread: SingleThreadExecutor runs FIFO, shutdown drains, shutdownNow drops") {
    {
        SingleThreadExecutor ex("single");
        Order                order;
        Counter              done;
        for (int i = 0; i < 100; ++i)
            REQUIRE(ex.execute([&, i] {
                order.push(i);
                done.add();
            }));
        REQUIRE(done.waitFor(100));
        auto ids = order.snapshot();
        for (int i = 0; i < 100; ++i) CHECK(ids[static_cast<std::size_t>(i)] == i);
    }
    {
        SingleThreadExecutor ex("drain");
        Gate                 release;
        Counter              started;
        std::atomic<int>     ran{0};
        REQUIRE(ex.execute([&] {
            started.add();
            release.wait();
        }));
        REQUIRE(started.waitFor(1));
        for (int i = 0; i < 5; ++i) REQUIRE(ex.execute([&] { ran.fetch_add(1); }));
        CHECK(ex.queueSize() == 5);
        ex.shutdown();
        CHECK_FALSE(ex.execute([&] { ran.fetch_add(1); }));
        release.open();
        ex.join();
        CHECK(ran.load() == 5); // queued work still ran
    }
    {
        SingleThreadExecutor ex("drop");
        Gate                 release;
        Counter              started;
        std::atomic<int>     ran{0};
        REQUIRE(ex.execute([&] {
            started.add();
            release.wait();
        }));
        REQUIRE(started.waitFor(1));
        for (int i = 0; i < 5; ++i) REQUIRE(ex.execute([&] { ran.fetch_add(1); }));
        ex.shutdownNow();
        CHECK(ex.queueSize() == 0);
        release.open();
        ex.join();
        CHECK(ran.load() == 0);
    }
}

// ---------------------------------------------------------------------------
// ThreadPools (ThreadPoolUtil)
// ---------------------------------------------------------------------------

TEST_CASE("thread: ThreadPools names, priorities and Java pausing predicates") {
    std::atomic<double> speed{0.0};
    std::atomic<bool>   pauseIfFast{true};
    std::atomic<bool>   limitUnhealthy{true};
    std::atomic<bool>   healthy{true};

    PauseConditions conditions;
    conditions.pauseIfMovingQuickly   = [&] { return pauseIfFast.load(); };
    conditions.avgCameraSpeed         = [&] { return speed.load(); };
    conditions.limitIfServerUnhealthy = [&] { return limitUnhealthy.load(); };
    conditions.serverThreadHealthy    = [&] { return healthy.load(); };

    ThreadPools pools(ThreadOptions{1, 1.0f}, conditions);

    CHECK(pools.networkCompression().name() == "Network Compression");
    CHECK(pools.fileHandler().name() == "IO");
    CHECK(pools.renderLoader().name() == "Render Loader");
    CHECK(pools.lodBuilder().name() == "LOD Builder");
    CHECK(pools.updatePropagator().name() == "Update Propagator");
    CHECK(pools.worldGen().name() == "World Gen");
    CHECK(pools.renderLoader().priority() == pools.lodBuilder().priority() + 1);
    CHECK(pools.cleanup().name() == "Cleanup");

    // camera speed: pause above ELYTRA (30) - 10 = 20 blocks/s, only when the config flag is on
    speed = 20.0;
    CHECK(pools.worldGenThreadsCanRun());
    CHECK(pools.updatePropagatorThreadsCanRun());
    speed = 20.5;
    CHECK_FALSE(pools.worldGenThreadsCanRun());
    CHECK_FALSE(pools.updatePropagatorThreadsCanRun());
    pauseIfFast = false;
    CHECK(pools.worldGenThreadsCanRun());
    CHECK(pools.updatePropagatorThreadsCanRun());

    // unhealthy server only limits world gen, and only if configured to
    healthy = false;
    CHECK_FALSE(pools.worldGenThreadsCanRun());
    CHECK(pools.updatePropagatorThreadsCanRun());
    limitUnhealthy = false;
    CHECK(pools.worldGenThreadsCanRun());

    // defaults with no conditions injected: never paused
    ThreadPools plain(ThreadOptions{1, 1.0f});
    CHECK(plain.worldGenThreadsCanRun());
    CHECK(plain.updatePropagatorThreadsCanRun());
}

TEST_CASE("thread: ThreadPools pauses on LOD builder backlog") {
    ThreadPools pools(ThreadOptions{1, 1.0f});

    // hold the single worker so queued LOD builder tasks stay queued
    Gate release;
    blockSingleWorker(pools.fileHandler(), release);

    std::atomic<int> ran{0};
    auto             task = [&] { ran.fetch_add(1); };

    // propagator pauses above poolSize * 200 = 200 queued
    for (int i = 0; i < 200; ++i) REQUIRE(pools.lodBuilder().execute(task));
    CHECK(pools.updatePropagatorThreadsCanRun());
    REQUIRE(pools.lodBuilder().execute(task));
    CHECK_FALSE(pools.updatePropagatorThreadsCanRun());
    CHECK(pools.worldGenThreadsCanRun()); // world gen only pauses above 1000

    // world gen and propagator both pause above 1000
    for (int i = 0; i < 800; ++i) REQUIRE(pools.lodBuilder().execute(task));
    CHECK(pools.lodBuilder().queueSize() == 1001);
    CHECK_FALSE(pools.worldGenThreadsCanRun());

    // paused executors do not run while the builder backlog exists
    std::atomic<int> genRan{0};
    REQUIRE(pools.worldGen().execute([&] { genRan.fetch_add(1); }));
    release.open();
    // drain the backlog by clearing instead of waiting for 1001 tasks
    pools.lodBuilder().clearQueue();
    CHECK(pools.worldGenThreadsCanRun());
    pools.picker().poke();

    // shutdown drops what is left and rejects new work on every picker executor
    pools.shutdown();
    pools.picker().joinAll(); // tasks above capture locals declared after `pools`
    CHECK_FALSE(pools.worldGen().execute([] {}));
    CHECK_FALSE(pools.networkClientHandler().execute([] {}));
    CHECK(pools.cleanup().execute([] {})); // cleanup is not shut down by shutdown()
}
