#pragma once

// Port of util/objects/RollingAverage.java. Thread safe.
//
// Java parity note: the Java implementation computes
//   currentSize = max(index + 1, currentSize)      (index already advanced)
// so the divisor is one larger than the number of samples until the ring is
// full (first add(v) yields average() == v/2). That is reproduced verbatim;
// it is only used for debug statistics. An empty average is NaN (0/0), as in Java.

#include "dh/util/Assert.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dh {

class RollingAverage {
public:
    explicit RollingAverage(int size) : maxSize_(size > 0 ? size : 1) {
        DH_ASSERT(size > 0, "RollingAverage size must be greater than 0");
        values_.assign(static_cast<std::size_t>(maxSize_), 0.0);
    }

    void add(double value) {
        std::lock_guard lock(mutex_);
        auto            i  = static_cast<std::size_t>(index_);
        sum_              -= values_[i];
        values_[i]         = value;
        sum_              += value;
        index_             = (index_ + 1) % maxSize_;
        currentSize_       = std::max(index_ + 1, currentSize_);
        ++lifetimeCount_;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        sum_           = 0;
        index_         = 0;
        currentSize_   = 0;
        lifetimeCount_ = 0;
        std::fill(values_.begin(), values_.end(), 0.0);
    }

    /// NaN when nothing has been added.
    double average() const {
        std::lock_guard lock(mutex_);
        return sum_ / static_cast<double>(currentSize_);
    }

    std::int64_t lifetimeCount() const {
        std::lock_guard lock(mutex_);
        return lifetimeCount_;
    }

private:
    mutable std::mutex  mutex_;
    std::vector<double> values_;
    int                 maxSize_;
    int                 currentSize_   = 0;
    int                 index_         = 0;
    double              sum_           = 0.0;
    std::int64_t        lifetimeCount_ = 0;
};

} // namespace dh
