#pragma once

// Java-semantics integer/float helpers. Every function here reproduces what the
// equivalent Java expression does, including the cases where plain C++ would be
// undefined behaviour (shift counts >= width, INT_MIN / -1, signed overflow, NaN
// to int casts).

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace dh {

//==================//
// wrapping int ops //
//==================//

/// Java `a + b` on int.
constexpr std::int32_t wadd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
/// Java `a - b` on int.
constexpr std::int32_t wsub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
/// Java `a * b` on int.
constexpr std::int32_t wmul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
/// Java `-a` on int (INT_MIN stays INT_MIN).
constexpr std::int32_t wneg(std::int32_t a) { return wsub(0, a); }
/// Java `Math.abs(int)` (abs(INT_MIN) == INT_MIN).
constexpr std::int32_t jabs32(std::int32_t a) { return a < 0 ? wneg(a) : a; }

constexpr std::int64_t wadd64(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
constexpr std::int64_t wsub64(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}
constexpr std::int64_t wmul64(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

//========//
// shifts //
//========//

/// Java `v << s` (int): shift count masked to 5 bits.
constexpr std::int32_t jshl32(std::int32_t v, std::int32_t s) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << (static_cast<std::uint32_t>(s) & 31u));
}
/// Java `v << s` (long): shift count masked to 6 bits.
constexpr std::int64_t jshl64(std::int64_t v, std::int64_t s) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(v) << (static_cast<std::uint64_t>(s) & 63u));
}
/// Java `v >> s` (int): arithmetic shift.
constexpr std::int32_t jshr32(std::int32_t v, std::int32_t s) {
    return v >> (static_cast<std::uint32_t>(s) & 31u); // arithmetic since C++20
}
/// Java `v >> s` (long).
constexpr std::int64_t jshr64(std::int64_t v, std::int64_t s) { return v >> (static_cast<std::uint64_t>(s) & 63u); }
/// Java `v >>> s` (int): logical shift.
constexpr std::int32_t jushr32(std::int32_t v, std::int32_t s) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) >> (static_cast<std::uint32_t>(s) & 31u));
}
/// Java `v >>> s` (long).
constexpr std::int64_t jushr64(std::int64_t v, std::int64_t s) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(v) >> (static_cast<std::uint64_t>(s) & 63u));
}

//==========//
// division //
//==========//

/// Java `a / b` on int: truncating, INT_MIN / -1 == INT_MIN. b must not be 0 (Java throws).
constexpr std::int32_t jdiv(std::int32_t a, std::int32_t b) {
    assert(b != 0);
    return b == -1 ? wneg(a) : a / b;
}
/// Java `a % b` on int: sign follows the dividend. b must not be 0.
constexpr std::int32_t jmod(std::int32_t a, std::int32_t b) {
    assert(b != 0);
    return b == -1 ? 0 : a % b;
}
constexpr std::int64_t jdiv64(std::int64_t a, std::int64_t b) {
    assert(b != 0);
    return b == -1 ? wsub64(0, a) : a / b;
}
constexpr std::int64_t jmod64(std::int64_t a, std::int64_t b) {
    assert(b != 0);
    return b == -1 ? 0 : a % b;
}

/// Java `Math.floorDiv(int, int)`.
constexpr std::int32_t jfloordiv(std::int32_t x, std::int32_t y) {
    std::int32_t q = jdiv(x, y);
    if ((x ^ y) < 0 && wmul(q, y) != x) {
        return q - 1;
    }
    return q;
}
/// Java `Math.floorMod(int, int)`.
constexpr std::int32_t jfloormod(std::int32_t x, std::int32_t y) {
    std::int32_t mod = jmod(x, y);
    if ((mod ^ y) < 0 && mod != 0) {
        mod = wadd(mod, y);
    }
    return mod;
}
constexpr std::int64_t jfloordiv64(std::int64_t x, std::int64_t y) {
    std::int64_t q = jdiv64(x, y);
    if ((x ^ y) < 0 && wmul64(q, y) != x) {
        return q - 1;
    }
    return q;
}
constexpr std::int64_t jfloormod64(std::int64_t x, std::int64_t y) {
    std::int64_t mod = jmod64(x, y);
    if ((mod ^ y) < 0 && mod != 0) {
        mod = wadd64(mod, y);
    }
    return mod;
}

//=======//
// casts //
//=======//

/// Java `(int) d`: NaN -> 0, saturating at INT_MIN / INT_MAX, otherwise truncation.
inline std::int32_t satcast_i32(double d) {
    if (std::isnan(d)) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return static_cast<std::int32_t>(d);
}
/// Java `(long) d`.
inline std::int64_t satcast_i64(double d) {
    if (std::isnan(d)) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<std::int64_t>(d);
}

//==============//
// BitShiftUtil //
//==============//

/// BitShiftUtil.powerOfTwo(int): `1 << value` (count masked like Java).
constexpr std::int32_t powerOfTwo(std::int32_t value) { return jshl32(1, value); }
/// BitShiftUtil.powerOfTwo(long): `1L << value`.
constexpr std::int64_t powerOfTwo64(std::int64_t value) { return jshl64(1, value); }
/// BitShiftUtil.half: `value >> 1` (floors for negatives).
constexpr std::int32_t half(std::int32_t value) { return value >> 1; }
constexpr std::int64_t half64(std::int64_t value) { return value >> 1; }
/// BitShiftUtil.divideByPowerOfTwo: `value >> power`.
constexpr std::int32_t divideByPowerOfTwo(std::int32_t value, std::int32_t power) { return jshr32(value, power); }
/// BitShiftUtil.square: `value << 1` (sic - this is a doubling, not a square).
constexpr std::int32_t square(std::int32_t value) { return jshl32(value, 1); }
constexpr std::int64_t square64(std::int64_t value) { return jshl64(value, 1); }
/// BitShiftUtil.pow: `value << power`.
constexpr std::int32_t shlPow(std::int32_t value, std::int32_t power) { return jshl32(value, power); }

constexpr int bitCount(std::uint32_t v) { return std::popcount(v); }
constexpr int bitCount(std::uint64_t v) { return std::popcount(v); }

//=====================//
// Math.min / Math.max //
//=====================//

/// Java `Math.max(float, float)`: NaN propagates, +0.0 > -0.0.
inline float jmaxf(float a, float b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0f && b == 0.0f) return std::signbit(a) ? b : a;
    return a >= b ? a : b;
}
inline float jminf(float a, float b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0f && b == 0.0f) return std::signbit(a) ? a : b;
    return a <= b ? a : b;
}
inline double jmaxd(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? b : a;
    return a >= b ? a : b;
}
inline double jmind(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? a : b;
    return a <= b ? a : b;
}

} // namespace dh
