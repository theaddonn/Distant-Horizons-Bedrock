#pragma once

#include <cstdint>

#include "dh/platform/BlockInfo.h"

/// Port of Java RenderDataPointUtil (core/util/RenderDataPointUtil.java).
///
/// Bit layout of a render data point (uint64):
///
///   bits 60..63  block material (4)      bits 20..31  max Y (12)
///   bits 56..59  alpha >> 4     (4)      bits  8..19  min Y (12)
///   bits 48..55  red            (8)      bits  4..7   block light (4)
///   bits 40..47  green          (8)      bits  0..3   sky light (4)
///   bits 32..39  blue           (8)
///
/// Intentional Java quirks reproduced here (do not "fix"):
///  * Y values are stored in 12 bits and silently truncated (`& 0xFFF`), although
///    MAX_WORLD_Y_SIZE (6095) is larger than 4095. Java only validates in dev builds.
///  * Alpha keeps only its top nibble; `alpha()` returns `(a4 << 4) | 0xF`.
///  * comparePriority uses a SIGNED arithmetic shift of the whole 64-bit word, so a
///    material >= 8 (bit 63 set) makes the value negative, and the difference is
///    truncated to int (RenderDataPointUtil.java:232).
///  * An all-zero word means "no data" (doesDataPointExist), while a word whose min and
///    max Y are both zero is a "void" (hasZeroHeight) datapoint.
namespace dh::rdp {

constexpr std::uint64_t EMPTY_DATA = 0;

/// Max min-Y (2031) + max height (4064). Larger than the 12-bit Y fields can hold.
constexpr int MAX_WORLD_Y_SIZE = 2031 + 4064;

constexpr int ALPHA_DOWNSIZE_SHIFT = 4;
constexpr int MATERIAL_SHIFT       = 60;
constexpr int COLOR_SHIFT          = 32;
constexpr int BLUE_SHIFT           = COLOR_SHIFT;
constexpr int GREEN_SHIFT          = BLUE_SHIFT + 8;
constexpr int RED_SHIFT            = GREEN_SHIFT + 8;
constexpr int ALPHA_SHIFT          = RED_SHIFT + 8;
constexpr int MAX_Y_SHIFT          = 20;
constexpr int MIN_Y_SHIFT          = 8;
constexpr int BLOCK_LIGHT_SHIFT    = 4;
constexpr int SKY_LIGHT_SHIFT      = 0;

constexpr std::uint64_t ALPHA_MASK         = 0xF;
constexpr std::uint64_t COLOR_CHANNEL_MASK = 0xFF;
constexpr std::uint64_t COLOR_MASK         = 0xFFFFFF;
constexpr std::uint64_t Y_MASK             = 0xFFF;
constexpr std::uint64_t MIN_MAX_Y_MASK     = 0xFFFFFF;
constexpr std::uint64_t LIGHT_MASK         = 0xF;
constexpr std::uint64_t MATERIAL_MASK      = 0xF;
constexpr int           COMPARE_SHIFT      = 7; // Java: 0b111 (RenderDataPointUtil.java:106)

constexpr std::uint64_t MAX_Y_SHIFTED_MASK = Y_MASK << MAX_Y_SHIFT;
constexpr std::uint64_t MIN_Y_SHIFTED_MASK = Y_MASK << MIN_Y_SHIFT;
constexpr std::uint64_t VOID_SETTER        = MAX_Y_SHIFTED_MASK | MIN_Y_SHIFTED_MASK;

namespace detail {
constexpr std::uint64_t widen(int v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); }
} // namespace detail

/// Java createDataPoint(alpha, red, green, blue, maxY, minY, lightSky, lightBlock, material).
/// No range validation (Java validates only in dev builds); out-of-range Y is truncated to 12 bits.
constexpr std::uint64_t create(
    int           alpha,
    int           red,
    int           green,
    int           blue,
    int           maxY,
    int           minY,
    int           skyLight,
    int           blockLight,
    BlockMaterial material
) {
    using detail::widen;
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(alpha) >> ALPHA_DOWNSIZE_SHIFT) << ALPHA_SHIFT)
         | ((widen(red) & COLOR_CHANNEL_MASK) << RED_SHIFT) | ((widen(green) & COLOR_CHANNEL_MASK) << GREEN_SHIFT)
         | ((widen(blue) & COLOR_CHANNEL_MASK) << BLUE_SHIFT) | ((widen(maxY) & Y_MASK) << MAX_Y_SHIFT)
         | ((widen(minY) & Y_MASK) << MIN_Y_SHIFT) | ((widen(blockLight) & LIGHT_MASK) << BLOCK_LIGHT_SHIFT)
         | ((widen(skyLight) & LIGHT_MASK) << SKY_LIGHT_SHIFT)
         | ((static_cast<std::uint64_t>(material) & MATERIAL_MASK) << MATERIAL_SHIFT);
}

/// Java createDataPoint(maxY, minY, color(argb), lightSky, lightBlock, material).
constexpr std::uint64_t
create(int maxY, int minY, std::uint32_t argb, int skyLight, int blockLight, BlockMaterial material) {
    return create(
        static_cast<int>(argb >> 24),
        static_cast<int>((argb >> 16) & 0xFF),
        static_cast<int>((argb >> 8) & 0xFF),
        static_cast<int>(argb & 0xFF),
        maxY,
        minY,
        skyLight,
        blockLight,
        material
    );
}

constexpr int yMax(std::uint64_t d) { return static_cast<int>((d >> MAX_Y_SHIFT) & Y_MASK); }
constexpr int yMin(std::uint64_t d) { return static_cast<int>((d >> MIN_Y_SHIFT) & Y_MASK); }

/// Java setYMin: replaces only the min-Y field (value truncated to 12 bits).
constexpr std::uint64_t setYMin(std::uint64_t d, int minY) {
    return (d & ~MIN_Y_SHIFTED_MASK) | ((detail::widen(minY) & Y_MASK) << MIN_Y_SHIFT);
}

/// Java getAlpha: 4 stored bits expanded as (a4 << 4) | 0xF, so 0..15 -> 15,31,...,255.
constexpr int alpha(std::uint64_t d) {
    return static_cast<int>((((d >> ALPHA_SHIFT) & ALPHA_MASK) << ALPHA_DOWNSIZE_SHIFT) | 0xF);
}
constexpr int red(std::uint64_t d) { return static_cast<int>((d >> RED_SHIFT) & COLOR_CHANNEL_MASK); }
constexpr int green(std::uint64_t d) { return static_cast<int>((d >> GREEN_SHIFT) & COLOR_CHANNEL_MASK); }
constexpr int blue(std::uint64_t d) { return static_cast<int>((d >> BLUE_SHIFT) & COLOR_CHANNEL_MASK); }

constexpr int skyLight(std::uint64_t d) { return static_cast<int>((d >> SKY_LIGHT_SHIFT) & LIGHT_MASK); }
constexpr int blockLight(std::uint64_t d) { return static_cast<int>((d >> BLOCK_LIGHT_SHIFT) & LIGHT_MASK); }

/// Raw 4-bit material id (Java getBlockMaterialId; always 0..15).
constexpr int           materialId(std::uint64_t d) { return static_cast<int>((d >> MATERIAL_SHIFT) & MATERIAL_MASK); }
constexpr BlockMaterial material(std::uint64_t d) { return static_cast<BlockMaterial>(materialId(d)); }

/// True if min Y and max Y are both zero ("void" datapoint). Tests the 24 Y bits only.
constexpr bool hasZeroHeight(std::uint64_t d) { return ((d >> MIN_Y_SHIFT) & MIN_MAX_Y_MASK) == 0; }
constexpr bool isVoid(std::uint64_t d) { return hasZeroHeight(d); }
constexpr bool doesDataPointExist(std::uint64_t d) { return d != EMPTY_DATA; }

/// Java getColor: ARGB with the quantised alpha from alpha().
constexpr std::uint32_t argb(std::uint64_t d) {
    return static_cast<std::uint32_t>((d >> COLOR_SHIFT) & COLOR_MASK) | (static_cast<std::uint32_t>(alpha(d)) << 24);
}

/// Java compareDatapointPriority: >0 if a should replace b, 0 if equal, <0 if b should replace a.
/// Signed arithmetic shift of the whole word (material >= 8 gives a negative value); the
/// difference is truncated to 32 bits like Java's `(int)` cast.
constexpr int comparePriority(std::uint64_t a, std::uint64_t b) {
    const std::int64_t  sa   = static_cast<std::int64_t>(a) >> COMPARE_SHIFT;
    const std::int64_t  sb   = static_cast<std::int64_t>(b) >> COMPARE_SHIFT;
    const std::uint64_t diff = static_cast<std::uint64_t>(sa) - static_cast<std::uint64_t>(sb);
    return static_cast<int>(static_cast<std::uint32_t>(diff));
}

} // namespace dh::rdp
