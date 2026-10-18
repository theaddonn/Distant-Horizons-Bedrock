#pragma once

// Time source for the core. Everything that needs "now" (timestamps persisted in
// the LOD store, rate limiters, bandwidth ticks, delayed saves) takes an IClock
// so tests can drive time deterministically with ManualClock.
//
//   nowMillis()    wall clock, milliseconds since the Unix epoch. Persisted in
//                  store metadata (Java: System.currentTimeMillis()).
//   steadyNanos()  monotonic, arbitrary origin; for durations/rate limiting only
//                  (Java: System.nanoTime()).

#include <chrono>
#include <cstdint>

namespace dh {

struct IClock {
    virtual ~IClock()                        = default;
    virtual std::int64_t nowMillis() const   = 0;
    virtual std::int64_t steadyNanos() const = 0;
};

/// Real clock backed by <chrono>.
class SystemClock final : public IClock {
public:
    std::int64_t nowMillis() const override {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
    std::int64_t steadyNanos() const override {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }
};

/// Test double: time moves only when told to. Both clocks advance together.
class ManualClock final : public IClock {
public:
    explicit ManualClock(std::int64_t startMillis = 1'700'000'000'000) : millis_(startMillis) {}

    std::int64_t nowMillis() const override { return millis_; }
    std::int64_t steadyNanos() const override { return steadyNanos_; }

    void advanceMillis(std::int64_t delta) {
        millis_      += delta;
        steadyNanos_ += delta * 1'000'000;
    }
    void advanceNanos(std::int64_t delta) {
        steadyNanos_ += delta;
        millis_      += delta / 1'000'000;
    }
    void setMillis(std::int64_t millis) { millis_ = millis; }

private:
    std::int64_t millis_;
    std::int64_t steadyNanos_ = 0;
};

} // namespace dh
