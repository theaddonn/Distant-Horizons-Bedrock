#pragma once

// Ports of util/ratelimiting/SupplierBased{Rate,Concurrency,RateAndConcurrency}Limiter.
//
// SupplierBasedRateLimiter used Guava's RateLimiter (SmoothBursty, maxBurst 1 s,
// created with an infinite rate and re-rated from the supplier on every call).
// GuavaRateLimiter below reproduces that algorithm over an injected IClock.
// A supplier value <= 0 makes the acquisition fail (Guava would throw).

#include "dh/platform/IClock.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>

namespace dh {

/// Guava SmoothBursty: permits accrue at `rate`/s, up to one second of burst is banked.
class GuavaRateLimiter {
public:
    explicit GuavaRateLimiter(const IClock& clock)
    : clock_(clock),
      nextFreeNanos_(static_cast<double>(clock.steadyNanos())) {}

    void setRate(double permitsPerSecond) {
        std::lock_guard lock(mutex_);
        double          now = static_cast<double>(clock_.steadyNanos());
        resync(now);
        stableIntervalNanos_ = 1e9 / permitsPerSecond;
        double oldMax        = maxPermits_;
        maxPermits_          = permitsPerSecond; // maxBurstSeconds = 1
        if (std::isinf(oldMax)) {
            storedPermits_ = maxPermits_;
        } else {
            storedPermits_ = (oldMax == 0.0) ? 0.0 : storedPermits_ * maxPermits_ / oldMax;
        }
    }

    bool tryAcquire(int permits) {
        std::lock_guard lock(mutex_);
        double          now = static_cast<double>(clock_.steadyNanos());
        if (nextFreeNanos_ > now) return false;
        resync(now);
        double toSpend  = std::min(static_cast<double>(permits), storedPermits_);
        double fresh    = static_cast<double>(permits) - toSpend;
        nextFreeNanos_ += fresh * stableIntervalNanos_;
        storedPermits_ -= toSpend;
        return true;
    }

private:
    void resync(double now) {
        if (now > nextFreeNanos_) {
            double newPermits = stableIntervalNanos_ == 0.0 ? std::numeric_limits<double>::infinity()
                                                            : (now - nextFreeNanos_) / stableIntervalNanos_;
            storedPermits_    = std::min(maxPermits_, storedPermits_ + newPermits);
            nextFreeNanos_    = now;
        }
    }

    const IClock& clock_;
    std::mutex    mutex_;
    double        storedPermits_ = 0.0;
    double        maxPermits_    = std::numeric_limits<double>::infinity(); // created with an infinite rate, as in Java
    double        stableIntervalNanos_ = 0.0;
    double        nextFreeNanos_;
};

inline const IClock& defaultLimiterClock() {
    static const SystemClock clock;
    return clock;
}

template <class TFailObj = int>
class SupplierBasedRateLimiter {
public:
    using Supplier = std::function<int()>;
    using OnFail   = std::function<void(const TFailObj&)>;

    explicit SupplierBasedRateLimiter(
        Supplier      maxRate,
        OnFail        onFailure = {},
        const IClock& clock     = defaultLimiterClock()
    )
    : maxRate_(std::move(maxRate)),
      onFailure_(std::move(onFailure)),
      limiter_(clock) {}

    bool tryAcquire(const TFailObj& failContext = TFailObj{}, int permits = 1) {
        if (!applyRate() || !limiter_.tryAcquire(permits)) {
            if (onFailure_) onFailure_(failContext);
            return false;
        }
        return true;
    }

    /// Prevents any acquisition for about one second.
    void acquireAll() { (void)acquireOrDrain(std::numeric_limits<int>::max()); }

    /// @return the number of permits acquired
    int acquireOrDrain(int requestedPermits) {
        if (!applyRate()) return 0;
        int acquired = 0;
        while (requestedPermits > 0 && limiter_.tryAcquire(1)) {
            ++acquired;
            --requestedPermits;
        }
        return acquired;
    }

private:
    bool applyRate() {
        int rate = maxRate_();
        if (rate <= 0) return false;
        limiter_.setRate(static_cast<double>(rate));
        return true;
    }

    Supplier         maxRate_;
    OnFail           onFailure_;
    GuavaRateLimiter limiter_;
};

template <class TFailObj = int>
class SupplierBasedConcurrencyLimiter {
public:
    using Supplier = std::function<int()>;
    using OnFail   = std::function<void(const TFailObj&)>;

    explicit SupplierBasedConcurrencyLimiter(Supplier maxConcurrent, OnFail onFailure = {})
    : maxConcurrent_(std::move(maxConcurrent)),
      onFailure_(std::move(onFailure)) {}

    bool tryAcquire(const TFailObj& context = TFailObj{}) {
        if (pending_.fetch_add(1) + 1 > maxConcurrent_()) {
            pending_.fetch_sub(1);
            if (onFailure_) onFailure_(context);
            return false;
        }
        return true;
    }

    void release() { pending_.fetch_sub(1); }

private:
    Supplier         maxConcurrent_;
    OnFail           onFailure_;
    std::atomic<int> pending_{0};
};

/// Both limits share the one supplier, as in Java.
template <class TFailObj = int>
class SupplierBasedRateAndConcurrencyLimiter {
public:
    using Supplier = std::function<int()>;
    using OnFail   = std::function<void(const TFailObj&)>;

    explicit SupplierBasedRateAndConcurrencyLimiter(
        Supplier      maxRate,
        OnFail        onFailure = {},
        const IClock& clock     = defaultLimiterClock()
    )
    : rate_(maxRate, onFailure, clock),
      concurrency_(maxRate, onFailure) {}

    bool tryAcquire(const TFailObj& context = TFailObj{}) {
        if (!concurrency_.tryAcquire(context)) return false;
        if (!rate_.tryAcquire(context)) {
            concurrency_.release();
            return false;
        }
        return true;
    }

    void release() { concurrency_.release(); }

private:
    SupplierBasedRateLimiter<TFailObj>        rate_;
    SupplierBasedConcurrencyLimiter<TFailObj> concurrency_;
};

} // namespace dh
