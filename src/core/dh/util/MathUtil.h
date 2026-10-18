#pragma once

// Port of coreapi/util/MathUtil.java.

#include <bit>
#include <cmath>
#include <cstdint>

#include "dh/util/Bits.h"

namespace dh::math {

/// MathUtil.clamp: NOTE the argument order is (min, value, max).
/// Java: Math.min(max, Math.max(value, min)) - "may behave strangely if min > max".
constexpr std::int32_t clamp(std::int32_t min, std::int32_t value, std::int32_t max) {
    const std::int32_t m = value > min ? value : min;
    return max < m ? max : m;
}
inline float  clamp(float min, float value, float max) { return jminf(max, jmaxf(value, min)); }
inline double clamp(double min, double value, double max) { return jmind(max, jmaxd(value, min)); }

/// Like Math.floorDiv but rounding up: -floorDiv(-value, divider).
constexpr std::int32_t ceilDiv(std::int32_t value, std::int32_t divider) {
    return wneg(jfloordiv(wneg(value), divider));
}

constexpr std::int8_t min(std::int8_t a, std::int8_t b) { return a < b ? a : b; }
constexpr std::int8_t max(std::int8_t a, std::int8_t b) { return a > b ? a : b; }

/// Quake-style fast inverse sqrt copied from Minecraft's MathHelper.
inline float fastInvSqrt(float numb) {
    const float  half = 0.5F * numb;
    std::int32_t i    = std::bit_cast<std::int32_t>(numb);
    i                 = wsub(1597463007, i >> 1);
    numb              = std::bit_cast<float>(i);
    return numb * (1.5F - half * numb * numb);
}

constexpr float        pow2(float x) { return x * x; }
constexpr double       pow2(double x) { return x * x; }
constexpr std::int32_t pow2(std::int32_t x) { return wmul(x, x); }
constexpr std::int64_t pow2(std::int64_t x) { return wmul64(x, x); }

/// Java `(int) (Math.log(numb) / Math.log(2))` (non-positive input gives NaN/-inf -> 0 / INT_MIN).
inline std::int32_t log2(std::int32_t numb) { return satcast_i32(std::log(static_cast<double>(numb)) / std::log(2.0)); }

} // namespace dh::math
