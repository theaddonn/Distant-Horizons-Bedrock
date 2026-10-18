#pragma once

// Port of coreapi/util/ColorUtil.java. Colours are DH format 0xAARRGGBB, carried
// in std::uint32_t (Java uses a signed int; the bit patterns are identical).
// Channel arguments that Java takes as int are int32 here and are shifted with
// wrapping semantics, so out-of-range inputs smear exactly as they do in Java.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include "dh/util/Bits.h"

namespace dh::color {

using Argb = std::uint32_t;

namespace detail {
constexpr Argb shl(std::int32_t v, int s) { return static_cast<Argb>(v) << s; }
} // namespace detail

constexpr Argb rgbToInt(std::int32_t red, std::int32_t green, std::int32_t blue) {
    return (0xFFu << 24) | detail::shl(red, 16) | detail::shl(green, 8) | static_cast<Argb>(blue);
}
constexpr Argb argb(std::int32_t alpha, std::int32_t red, std::int32_t green, std::int32_t blue) {
    return detail::shl(alpha, 24) | detail::shl(red, 16) | detail::shl(green, 8) | static_cast<Argb>(blue);
}
/// argbToInt(float, float, float, float): each channel is `(int) (c * 255f)` (float multiply, saturating cast).
inline Argb argb(float alpha, float red, float green, float blue) {
    return argb(
        satcast_i32(alpha * 255.0f),
        satcast_i32(red * 255.0f),
        satcast_i32(green * 255.0f),
        satcast_i32(blue * 255.0f)
    );
}

constexpr Argb INVISIBLE   = argb(0, 0, 0, 0);
constexpr Argb BLACK       = rgbToInt(0, 0, 0);
constexpr Argb WHITE       = rgbToInt(255, 255, 255);
constexpr Argb RED         = rgbToInt(255, 0, 0);
constexpr Argb DARK_RED    = rgbToInt(100, 0, 0);
constexpr Argb GREEN       = rgbToInt(0, 255, 0);
constexpr Argb DARK_GREEN  = rgbToInt(80, 140, 80);
constexpr Argb BLUE        = rgbToInt(0, 0, 255);
constexpr Argb YELLOW      = rgbToInt(255, 255, 0);
constexpr Argb CYAN        = rgbToInt(0, 255, 255);
constexpr Argb MAGENTA     = rgbToInt(255, 0, 255);
constexpr Argb ORANGE      = rgbToInt(255, 128, 0);
constexpr Argb DARK_ORANGE = rgbToInt(125, 62, 0);
constexpr Argb TAN         = rgbToInt(183, 165, 119);
constexpr Argb PINK        = rgbToInt(255, 128, 128);
constexpr Argb HOT_PINK    = rgbToInt(255, 105, 180);
constexpr Argb GRAY        = rgbToInt(128, 128, 128);
constexpr Argb LIGHT_GRAY  = rgbToInt(192, 192, 192);
constexpr Argb DARK_GRAY   = rgbToInt(64, 64, 64);
constexpr Argb BROWN       = rgbToInt(68, 46, 24);
constexpr Argb LIGHT_BROWN = rgbToInt(130, 112, 67);
constexpr Argb PURPLE      = rgbToInt(128, 0, 128);

/// Each returns 0..255.
constexpr std::int32_t getAlpha(Argb c) { return static_cast<std::int32_t>((c >> 24) & 0xFFu); }
constexpr std::int32_t getRed(Argb c) { return static_cast<std::int32_t>((c >> 16) & 0xFFu); }
constexpr std::int32_t getGreen(Argb c) { return static_cast<std::int32_t>((c >> 8) & 0xFFu); }
constexpr std::int32_t getBlue(Argb c) { return static_cast<std::int32_t>(c & 0xFFu); }

constexpr Argb setAlpha(Argb c, std::int32_t v) { return argb(v, getRed(c), getGreen(c), getBlue(c)); }
constexpr Argb setRed(Argb c, std::int32_t v) { return argb(getAlpha(c), v, getGreen(c), getBlue(c)); }
constexpr Argb setGreen(Argb c, std::int32_t v) { return argb(getAlpha(c), getRed(c), v, getBlue(c)); }
constexpr Argb setBlue(Argb c, std::int32_t v) { return argb(getAlpha(c), getRed(c), getGreen(c), v); }

/// applyShade(int color, int shade): additive shade, clamped to 0..255 per channel.
constexpr Argb applyShadeInt(Argb c, std::int32_t shade) {
    auto lo = [&](std::int32_t ch) { return wadd(ch, shade) > 0 ? wadd(ch, shade) : 0; };
    auto hi = [&](std::int32_t ch) { return wadd(ch, shade) < 255 ? wadd(ch, shade) : 255; };
    if (shade < 0) {
        return detail::shl(getAlpha(c), 24) | detail::shl(lo(getRed(c)), 16) | detail::shl(lo(getGreen(c)), 8)
             | static_cast<Argb>(lo(getBlue(c)));
    }
    return detail::shl(getAlpha(c), 24) | detail::shl(hi(getRed(c)), 16) | detail::shl(hi(getGreen(c)), 8)
         | static_cast<Argb>(hi(getBlue(c)));
}

/// applyShade(int color, float shade) - ColorUtil.java:112. Float multiply of
/// (channel * shade), Math.max(.,0) below 1 / Math.min(.,255) otherwise, then a
/// truncating (int) cast per channel. Alpha is passed through unchanged.
inline Argb applyShade(Argb c, float shade) {
    auto ch = [&](std::int32_t v) -> std::int32_t {
        const float scaled = static_cast<float>(v) * shade;
        return satcast_i32(shade < 1.0f ? jmaxf(scaled, 0.0f) : jminf(scaled, 255.0f));
    };
    return detail::shl(getAlpha(c), 24) | detail::shl(ch(getRed(c)), 16) | detail::shl(ch(getGreen(c)), 8)
         | static_cast<Argb>(ch(getBlue(c)));
}

/// Multiply ARGB with RGB colours (alpha of the first is kept).
constexpr Argb multiplyARGBwithRGB(Argb a, Argb rgb) {
    return detail::shl(getAlpha(a), 24) | detail::shl(getRed(a) * getRed(rgb) / 255, 16)
         | detail::shl(getGreen(a) * getGreen(rgb) / 255, 8) | static_cast<Argb>(getBlue(a) * getBlue(rgb) / 255);
}
/// Multiply 2 ARGB colours.
constexpr Argb multiplyARGBwithARGB(Argb a, Argb b) {
    return detail::shl(getAlpha(a) * getAlpha(b) / 255, 24) | detail::shl(getRed(a) * getRed(b) / 255, 16)
         | detail::shl(getGreen(a) * getGreen(b) / 255, 8) | static_cast<Argb>(getBlue(a) * getBlue(b) / 255);
}

/// {alpha 0..1, hue 0..360, saturation 0..1, value 0..1}; all float math as in Java.
inline std::array<float, 4> argbToAhsv(Argb color) {
    const float a = static_cast<float>(getAlpha(color)) / 255.0f;
    const float r = static_cast<float>(getRed(color)) / 255.0f;
    const float g = static_cast<float>(getGreen(color)) / 255.0f;
    const float b = static_cast<float>(getBlue(color)) / 255.0f;
    float       h, s, v;
    const float min = jminf(jminf(r, g), b);
    const float max = jmaxf(jmaxf(r, g), b);

    v                 = max;
    const float delta = max - min;
    if (max != 0.0f) {
        s = delta / max;
    } else {
        return {a, 0.0f, 0.0f, 0.0f};
    }
    if (delta == 0.0f) {
        h = 0.0f;
    } else {
        if (r == max) h = (g - b) / delta;
        else if (g == max) h = 2.0f + (b - r) / delta;
        else h = 4.0f + (r - g) / delta;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }
    return {a, h, s, v};
}

/// Port of ahsvToArgb, including its quirk `if (h > 360) h -= 350` (sic, not 360).
inline Argb ahsvToArgb(float a, float h, float s, float v) {
    if (a > 1.0f) a = 1.0f;
    if (h > 360.0f) h -= 350.0f;
    if (s > 1.0f) s = 1.0f;
    if (v > 1.0f) v = 1.0f;

    if (s == 0.0f) {
        return argb(a, v, v, v);
    }
    h                    /= 60.0f;
    const std::int32_t i  = satcast_i32(std::floor(static_cast<double>(h)));
    const float        f  = h - static_cast<float>(i);
    const float        p  = v * (1.0f - s);
    const float        q  = v * (1.0f - s * f);
    const float        t  = v * (1.0f - s * (1.0f - f));

    switch (i) {
    case 0:
        return argb(a, v, t, p);
    case 1:
        return argb(a, q, v, p);
    case 2:
        return argb(a, p, v, t);
    case 3:
        return argb(a, p, q, v);
    case 4:
        return argb(a, t, p, v);
    default:
        return argb(a, v, p, q);
    }
}

/// "A:ff,R:..,G:..,B:.." with lower-case hex without padding (Integer.toHexString).
std::string toHexString(Argb color);
/// "A:255,R:..,G:..,B:.."
std::string toString(Argb color);

} // namespace dh::color
