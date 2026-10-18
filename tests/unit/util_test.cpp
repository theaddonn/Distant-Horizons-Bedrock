#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "dh/util/Bits.h"
#include "dh/util/ByteIO.h"
#include "dh/util/Color.h"
#include "dh/util/MathUtil.h"
#include "dh/util/Varint.h"

using namespace dh;

//===========//
// VarintTest //
//===========//

namespace {

void testZigZagEncoding(std::int32_t value) {
    const std::uint32_t encodedValue = varint::zigzag(value);
    const std::int32_t  decodedValue = varint::unzigzag(encodedValue);
    CHECK(value == decodedValue);
}

void testSingleVarint(std::int32_t value) {
    // write to stream
    std::vector<std::byte> bytes;
    const std::uint32_t    encodedValue = varint::zigzag(value);
    REQUIRE(varint::writeVarint(bytes, encodedValue)); // varint requires zig-zag encoding to function

    // read stream
    std::size_t off  = 0;
    auto        read = varint::readVarint(bytes, off);
    REQUIRE(read.has_value());
    CHECK(off == bytes.size());
    CHECK(varint::unzigzag(*read) == value);
}

} // namespace

TEST_CASE("VarintTest.Test") {
    CHECK(0x80 == 128);

    // zig zag encoding is needed for varint handling, so test it first
    for (int i = -256; i < 256; i++) {
        testZigZagEncoding(i);
    }
    for (int i = -256; i < 256; i++) {
        testSingleVarint(i);
    }
}

TEST_CASE("varint: exact wire format and error handling") {
    std::vector<std::byte> b;
    REQUIRE(varint::writeVarint(b, 0));
    REQUIRE(varint::writeVarint(b, 127));
    REQUIRE(varint::writeVarint(b, 128));
    REQUIRE(varint::writeVarint(b, 300));
    REQUIRE(varint::writeVarint(b, 0x7FFFFFFFu));
    const std::vector<std::uint8_t> expected = {0x00, 0x7F, 0x80, 0x01, 0xAC, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
    REQUIRE(b.size() == expected.size());
    for (std::size_t i = 0; i < b.size(); i++) CHECK(static_cast<std::uint8_t>(b[i]) == expected[i]);

    // "varint only accepts positive values": anything with the sign bit set is rejected, nothing written
    std::vector<std::byte> r;
    CHECK_FALSE(varint::writeVarint(r, 0x80000000u));
    CHECK_FALSE(varint::writeVarint(r, 0xFFFFFFFFu));
    CHECK(r.empty());

    // decode
    std::size_t off = 0;
    CHECK(*varint::readVarint(b, off) == 0);
    CHECK(*varint::readVarint(b, off) == 127);
    CHECK(*varint::readVarint(b, off) == 128);
    CHECK(*varint::readVarint(b, off) == 300);
    CHECK(*varint::readVarint(b, off) == 0x7FFFFFFFu);
    CHECK(off == b.size());
    CHECK_FALSE(varint::readVarint(b, off).has_value()); // truncated (empty)

    // truncated continuation: offset untouched
    std::vector<std::byte> trunc = {std::byte{0x80}, std::byte{0x80}};
    off                          = 0;
    CHECK_FALSE(varint::readVarint(trunc, off).has_value());
    CHECK(off == 0);

    // shift cap 32: five continuation bytes (shifts 0,7,14,21,28) then a sixth read at shift 35 -> invalid
    std::vector<std::byte> bad(6, std::byte{0x80});
    bad.back() = std::byte{0x01};
    off        = 0;
    CHECK_FALSE(varint::readVarint(bad, off).has_value());
    CHECK(off == 0);
    // a 5th byte may carry bits above 32; they are silently dropped (Java: (b & 127) << 28 in int)
    std::vector<std::byte> five = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    off                         = 0;
    CHECK(*varint::readVarint(five, off) == 0xFFFFFFFFu);
}

TEST_CASE("zigzag identities") {
    CHECK(varint::zigzag(0) == 0);
    CHECK(varint::zigzag(-1) == 1);
    CHECK(varint::zigzag(1) == 2);
    CHECK(varint::zigzag(-2) == 3);
    CHECK(varint::zigzag(2) == 4);
    CHECK(varint::zigzag(INT32_MAX) == 0xFFFFFFFEu);
    CHECK(varint::zigzag(INT32_MIN) == 0xFFFFFFFFu);
    CHECK(varint::unzigzag(0xFFFFFFFFu) == INT32_MIN);

    std::mt19937 rng(7);
    for (int i = 0; i < 1000000; i++) {
        const std::int32_t  n = static_cast<std::int32_t>(rng());
        const std::uint32_t z = varint::zigzag(n);
        // Java reference: (n << 1) ^ (n >> 31)
        REQUIRE(z == (static_cast<std::uint32_t>(n) << 1 ^ static_cast<std::uint32_t>(n >> 31)));
        REQUIRE(varint::unzigzag(z) == n);
        // low bit is the sign; magnitude relation
        REQUIRE((z & 1u) == (n < 0 ? 1u : 0u));
        if (n >= 0) REQUIRE(z == static_cast<std::uint32_t>(n) * 2u);
        // Java decode reference: (z >>> 1) ^ -(z & 1)
        REQUIRE(static_cast<std::uint32_t>(varint::unzigzag(z)) == ((z >> 1) ^ (0u - (z & 1u))));
    }
}

TEST_CASE("property: varint round trip for 1e6 random non-negative values") {
    std::mt19937_64        rng(1234);
    std::vector<std::byte> buf;
    for (int i = 0; i < 1000000; i++) {
        // spread across all magnitudes, not just the top bits
        const std::uint32_t bits   = static_cast<std::uint32_t>(rng() % 32);
        std::uint32_t       v      = static_cast<std::uint32_t>(rng()) & 0x7FFFFFFFu;
        v                        >>= (bits > 31 ? 31 : bits);
        buf.clear();
        REQUIRE(varint::writeVarint(buf, v));
        std::size_t off = 0;
        auto        r   = varint::readVarint(buf, off);
        REQUIRE(r.has_value());
        REQUIRE(*r == v);
        REQUIRE(off == buf.size());
    }
}

TEST_CASE("property: zigzag + varint round trip for |n| < 2^30") {
    std::mt19937_64                             rng(99);
    std::uniform_int_distribution<std::int32_t> d(-(1 << 30) + 1, (1 << 30) - 1);
    std::vector<std::byte>                      buf;
    for (int i = 0; i < 200000; i++) {
        const std::int32_t n = d(rng);
        buf.clear();
        REQUIRE(varint::writeVarint(buf, varint::zigzag(n)));
        std::size_t off = 0;
        REQUIRE(varint::unzigzag(*varint::readVarint(buf, off)) == n);
    }
    // |n| >= 2^30 zigzags into the sign bit and Java's writeVarint rejects it
    std::vector<std::byte> b;
    CHECK_FALSE(varint::writeVarint(b, varint::zigzag(1 << 30)));
    CHECK_FALSE(varint::writeVarint(b, varint::zigzag(-(1 << 30) - 1)));
}

//======//
// Bits //
//======//

TEST_CASE("Bits: Java shift semantics") {
    CHECK(jshl32(1, 32) == 1); // count masked to 5 bits
    CHECK(jshl32(1, 31) == INT32_MIN);
    CHECK(jshl32(1, -1) == INT32_MIN); // -1 & 31 == 31
    CHECK(jshl32(3, 31) == INT32_MIN);
    CHECK(jshl64(1, 64) == 1);
    CHECK(jshl64(1, 63) == INT64_MIN);
    CHECK(jshr32(-8, 1) == -4);
    CHECK(jshr32(-1, 33) == -1);
    CHECK(jshr32(INT32_MIN, 31) == -1);
    CHECK(jshr64(INT64_MIN, 63) == -1);
    CHECK(jushr32(-1, 1) == INT32_MAX);
    CHECK(jushr32(-1, 32) == -1); // shift by 32 is shift by 0
    CHECK(jushr32(-1, 28) == 15);
    CHECK(jushr64(-1, 1) == INT64_MAX);
    CHECK(jushr64(-1, 63) == 1);
    CHECK(powerOfTwo(0) == 1);
    CHECK(powerOfTwo(10) == 1024);
    CHECK(powerOfTwo(31) == INT32_MIN);
    CHECK(powerOfTwo(32) == 1);
    CHECK(powerOfTwo64(40) == (std::int64_t{1} << 40));
    CHECK(half(-3) == -2);
    CHECK(half(3) == 1);
    CHECK(square(5) == 10); // BitShiftUtil.square is a doubling
    CHECK(bitCount(0xF0F0u) == 8);
    CHECK(bitCount(std::uint64_t{0xFFFFFFFFFFFFFFFFull}) == 64);
}

TEST_CASE("Bits: Java division semantics") {
    CHECK(jdiv(7, 2) == 3);
    CHECK(jdiv(-7, 2) == -3); // truncating
    CHECK(jdiv(7, -2) == -3);
    CHECK(jdiv(INT32_MIN, -1) == INT32_MIN);
    CHECK(jmod(-7, 2) == -1); // sign follows dividend
    CHECK(jmod(7, -2) == 1);
    CHECK(jmod(INT32_MIN, -1) == 0);
    CHECK(jfloordiv(7, 2) == 3);
    CHECK(jfloordiv(-7, 2) == -4);
    CHECK(jfloordiv(7, -2) == -4);
    CHECK(jfloordiv(-7, -2) == 3);
    CHECK(jfloordiv(-8, 2) == -4);
    CHECK(jfloordiv(INT32_MIN, -1) == INT32_MIN);
    CHECK(jfloormod(-7, 2) == 1);
    CHECK(jfloormod(7, -2) == -1);
    CHECK(jfloormod(-8, 2) == 0);
    CHECK(jfloormod(-1, 64) == 63);
    CHECK(jfloordiv64(-7, 2) == -4);
    CHECK(jfloormod64(-7, 2) == 1);
    CHECK(wadd(INT32_MAX, 1) == INT32_MIN);
    CHECK(wmul(1 << 20, 1 << 20) == 0);
    CHECK(jabs32(INT32_MIN) == INT32_MIN);

    std::mt19937 rng(5);
    for (int i = 0; i < 200000; i++) {
        const std::int32_t a = static_cast<std::int32_t>(rng());
        std::int32_t       b = static_cast<std::int32_t>(rng() % 2001) - 1000;
        if (b == 0) b = 7;
        REQUIRE(jfloordiv(a, b) * static_cast<std::int64_t>(b) + jfloormod(a, b) == a);
        const std::int32_t m = jfloormod(a, b);
        REQUIRE((b > 0 ? (m >= 0 && m < b) : (m <= 0 && m > b)));
    }
}

TEST_CASE("Bits: satcast_i32 is Java (int) cast") {
    CHECK(satcast_i32(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(satcast_i32(std::numeric_limits<double>::infinity()) == INT32_MAX);
    CHECK(satcast_i32(-std::numeric_limits<double>::infinity()) == INT32_MIN);
    CHECK(satcast_i32(3.99) == 3);
    CHECK(satcast_i32(-3.99) == -3); // truncation toward zero
    CHECK(satcast_i32(2147483647.0) == INT32_MAX);
    CHECK(satcast_i32(2147483648.0) == INT32_MAX);
    CHECK(satcast_i32(-2147483648.0) == INT32_MIN);
    CHECK(satcast_i32(-2147483649.0) == INT32_MIN);
    CHECK(satcast_i32(1e30) == INT32_MAX);
    CHECK(satcast_i64(1e30) == INT64_MAX);
    CHECK(satcast_i64(-1e30) == INT64_MIN);
    CHECK(satcast_i64(std::numeric_limits<double>::quiet_NaN()) == 0);
}

TEST_CASE("Bits: Math.min/max float edge cases") {
    CHECK(std::isnan(jmaxf(std::numeric_limits<float>::quiet_NaN(), 1.0f)));
    CHECK(std::isnan(jminf(1.0f, std::numeric_limits<float>::quiet_NaN())));
    CHECK_FALSE(std::signbit(jmaxf(-0.0f, 0.0f)));
    CHECK(std::signbit(jminf(-0.0f, 0.0f)));
    CHECK(jmaxf(1.0f, 2.0f) == 2.0f);
    CHECK(jmind(1.0, 2.0) == 1.0);
}

//========//
// ByteIO //
//========//

TEST_CASE("ByteIO: endianness and bounds checks") {
    std::vector<std::byte> buf;
    ByteWriter             w(buf);
    w.putU8(0xAB);
    w.putI8(-2);
    w.putU16LE(0x1234);
    w.putU32LE(0x01020304);
    w.putU16BE(0x1234);
    w.putU32BE(0x01020304);
    w.putI64BE(-2);
    w.putI64LE(-3);
    w.putF32BE(1.5f);
    w.putF64LE(-2.25);
    w.putI32BE(INT32_MIN);

    auto u8 = [&](std::size_t i) { return static_cast<std::uint8_t>(buf[i]); };
    CHECK(u8(0) == 0xAB);
    CHECK(u8(1) == 0xFE);
    CHECK(u8(2) == 0x34);
    CHECK(u8(3) == 0x12);
    CHECK(u8(4) == 0x04);
    CHECK(u8(7) == 0x01);
    CHECK(u8(8) == 0x12);
    CHECK(u8(9) == 0x34);
    CHECK(u8(10) == 0x01);
    CHECK(u8(13) == 0x04);

    ByteReader r(buf);
    CHECK(*r.getU8() == 0xAB);
    CHECK(*r.getI8() == -2);
    CHECK(*r.getU16LE() == 0x1234);
    CHECK(*r.getU32LE() == 0x01020304u);
    CHECK(*r.getU16BE() == 0x1234);
    CHECK(*r.getU32BE() == 0x01020304u);
    CHECK(*r.getI64BE() == -2);
    CHECK(*r.getI64LE() == -3);
    CHECK(*r.getF32BE() == 1.5f);
    CHECK(*r.getF64LE() == -2.25);
    CHECK(*r.getI32BE() == INT32_MIN);
    CHECK(r.atEnd());
    CHECK_FALSE(r.getU8().has_value());
    CHECK_FALSE(r.getU32LE().has_value());

    // short reads fail without consuming
    ByteReader s(std::span<const std::byte>(buf.data(), 3));
    CHECK_FALSE(s.getU32LE().has_value());
    CHECK(s.position() == 0);
    CHECK(*s.getU16LE() == 0xFEAB);
    CHECK_FALSE(s.getBytes(2).has_value());
    CHECK(s.getBytes(1).has_value());
    CHECK_FALSE(s.skip(1));
    CHECK_FALSE(s.seek(4));
    CHECK(s.seek(0));
}

//=========//
// MathUtil //
//=========//

TEST_CASE("MathUtil: clamp argument order is (min, value, max)") {
    CHECK(math::clamp(0, 5, 3) == 3);
    CHECK(math::clamp(0, -5, 3) == 0);
    CHECK(math::clamp(0, 2, 3) == 2);
    // min > max: Math.min(max, Math.max(value, min)) -> max wins
    CHECK(math::clamp(5, 0, 3) == 3);
    CHECK(math::clamp(0.0f, 1.5f, 1.0f) == 1.0f);
    CHECK(math::clamp(0.0, -1.5, 1.0) == 0.0);
    CHECK(std::isnan(math::clamp(0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f)));
}

TEST_CASE("MathUtil: ceilDiv / log2 / pow2 / fastInvSqrt") {
    CHECK(math::ceilDiv(7, 2) == 4);
    CHECK(math::ceilDiv(8, 2) == 4);
    CHECK(math::ceilDiv(-7, 2) == -3);
    CHECK(math::ceilDiv(-8, 2) == -4);
    CHECK(math::ceilDiv(1, 64) == 1);
    CHECK(math::ceilDiv(0, 64) == 0);

    for (int i = 0; i < 31; i++) {
        CHECK(math::log2(1 << i) == i);
    }
    CHECK(math::log2(1000) == 9);
    CHECK(math::log2(0) == INT32_MIN); // (int) -Infinity

    CHECK(math::pow2(3) == 9);
    CHECK(math::pow2(std::int64_t{100000}) == 10000000000LL);
    CHECK(math::pow2(1.5) == 2.25);
    CHECK(math::pow2(65536) == 0); // int overflow wraps
    CHECK(math::min(std::int8_t{-3}, std::int8_t{2}) == -3);
    CHECK(math::max(std::int8_t{-3}, std::int8_t{2}) == 2);
    CHECK(math::fastInvSqrt(4.0f) == doctest::Approx(0.5f).epsilon(0.01));
}

//======//
// Color //
//======//

TEST_CASE("Color: packing and channel access") {
    using namespace color;
    const Argb c = argb(0xAA, 0x11, 0x22, 0x33);
    CHECK(c == 0xAA112233u);
    CHECK(getAlpha(c) == 0xAA);
    CHECK(getRed(c) == 0x11);
    CHECK(getGreen(c) == 0x22);
    CHECK(getBlue(c) == 0x33);
    CHECK(setAlpha(c, 0x01) == 0x01112233u);
    CHECK(setRed(c, 0xFF) == 0xAAFF2233u);
    CHECK(setGreen(c, 0xFF) == 0xAA11FF33u);
    CHECK(setBlue(c, 0xFF) == 0xAA1122FFu);
    CHECK(rgbToInt(1, 2, 3) == 0xFF010203u);
    CHECK(WHITE == 0xFFFFFFFFu);
    CHECK(INVISIBLE == 0u);
    CHECK(HOT_PINK == 0xFFFF69B4u);
    // float overload: (int) (c * 255f), truncating
    CHECK(argb(1.0f, 0.5f, 0.0f, 0.999f) == argb(255, 127, 0, 254));
    // out-of-range ints smear exactly like Java's (red << 16)
    CHECK(argb(0, -1, 0, 0) == 0xFFFF0000u);
    CHECK(toHexString(c) == "A:aa,R:11,G:22,B:33");
    CHECK(toString(c) == "A:170,R:17,G:34,B:51");
}

TEST_CASE("Color: applyShade(int, float) float math + truncation (ColorUtil.java:112)") {
    using namespace color;
    const Argb c = 0xFF804020u;
    CHECK(applyShade(c, 0.5f) == 0xFF402010u);
    CHECK(applyShade(c, 0.0f) == 0xFF000000u);
    CHECK(applyShade(c, 1.0f) == c);
    CHECK(applyShade(c, 1.5f) == 0xFFC06030u);
    CHECK(applyShade(c, 2.0f) == 0xFFFF8040u); // red 256 clamped to 255
    CHECK(applyShade(c, -1.0f) == 0xFF000000u);
    CHECK(applyShade(c, std::numeric_limits<float>::quiet_NaN()) == 0xFF000000u); // NaN -> (int) 0
    CHECK(applyShade(c, 1e30f) == 0xFFFFFFFFu);
    // truncation not rounding: 255 * 0.999f = 254.745 -> 254, 3 * 0.9f -> 2
    CHECK(applyShade(0xFFFF0303u, 0.999f) == 0xFFFE0202u);
    // alpha is untouched
    CHECK(getAlpha(applyShade(0x7F808080u, 0.5f)) == 0x7F);
    // shade 0.7f in float: 100 * 0.7f = 70.0000004... -> 70 ; 10 * 0.7f = 7.0000001 -> 7
    CHECK(applyShade(0xFF640A00u, 0.7f) == 0xFF460700u);
    // float multiply, not double: (float)0.1 * 30 == 3.0000001192... in float -> 3 (but 29*0.1f -> 2.9)
    CHECK(applyShade(0xFF1D1E00u, 0.1f) == 0xFF020300u);
}

TEST_CASE("Color: applyShade(int, int) and multiply") {
    using namespace color;
    CHECK(applyShadeInt(0xFF102030u, 16) == 0xFF203040u);
    CHECK(applyShadeInt(0xFF102030u, -16) == 0xFF001020u);
    CHECK(applyShadeInt(0xFFF0F0F0u, 32) == 0xFFFFFFFFu);
    CHECK(multiplyARGBwithRGB(0xFF808080u, 0x00FF8000u) == 0xFF804000u);
    CHECK(multiplyARGBwithARGB(0x80FFFFFFu, 0x80808080u) == 0x40808080u);
}

TEST_CASE("Color: ahsv conversions") {
    using namespace color;
    auto red = argbToAhsv(RED);
    CHECK(red[0] == 1.0f);
    CHECK(red[1] == 0.0f);
    CHECK(red[2] == 1.0f);
    CHECK(red[3] == 1.0f);
    auto green = argbToAhsv(GREEN);
    CHECK(green[1] == 120.0f);
    auto blue = argbToAhsv(BLUE);
    CHECK(blue[1] == 240.0f);
    auto black = argbToAhsv(0xFF000000u);
    CHECK(black[0] == 1.0f);
    CHECK(black[1] == 0.0f);
    CHECK(black[2] == 0.0f);
    CHECK(black[3] == 0.0f);
    auto gray = argbToAhsv(GRAY);
    CHECK(gray[1] == 0.0f);
    CHECK(gray[2] == 0.0f);
    // magenta: r == max, h = (0 - 1)/1 * 60 = -60 -> +360 = 300
    CHECK(argbToAhsv(MAGENTA)[1] == 300.0f);

    CHECK(ahsvToArgb(1.0f, 0.0f, 1.0f, 1.0f) == RED);
    CHECK(ahsvToArgb(1.0f, 120.0f, 1.0f, 1.0f) == GREEN);
    CHECK(ahsvToArgb(1.0f, 240.0f, 1.0f, 1.0f) == BLUE);
    CHECK(ahsvToArgb(1.0f, 0.0f, 0.0f, 1.0f) == WHITE);
    CHECK(ahsvToArgb(1.0f, 60.0f, 1.0f, 1.0f) == YELLOW);
    // quirk: h > 360 subtracts 350 (not 360): 361 -> 11
    CHECK(ahsvToArgb(1.0f, 361.0f, 1.0f, 1.0f) == ahsvToArgb(1.0f, 11.0f, 1.0f, 1.0f));
    // negative hue -> floor < 0 -> default branch with f = h - i = 5/6: (v, p, q) = (1, 0, 1/6)
    CHECK(ahsvToArgb(1.0f, -10.0f, 1.0f, 1.0f) == argb(255, 255, 0, 42));
}
