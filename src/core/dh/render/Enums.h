#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace dh {

/// Port of EDhApiVerticalQuality. Tables are copied verbatim from Java; index = data detail level.
enum class VerticalQuality : std::uint8_t {
    HeightMap,
    Low,
    Medium,
    High,
    VeryHigh,
    Extreme,
    PixelArt,
};

inline constexpr int VERTICAL_QUALITY_DETAIL_COUNT = 11;

inline constexpr std::array<std::array<int, VERTICAL_QUALITY_DETAIL_COUNT>, 7> VERTICAL_QUALITY_SLICES{
    {
     /* HEIGHT_MAP */ {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
     /* LOW        */ {4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 1},
     /* MEDIUM     */ {6, 6, 6, 4, 4, 4, 4, 4, 4, 4, 1},
     /* HIGH       */ {16, 16, 12, 12, 8, 8, 8, 8, 8, 8, 1},
     /* VERY_HIGH  */ {32, 16, 12, 12, 12, 12, 12, 12, 12, 12, 1},
     /* EXTREME    */ {64, 32, 32, 32, 16, 16, 16, 16, 16, 16, 1},
     /* PIXEL_ART  */ {512, 256, 128, 64, 32, 32, 16, 16, 16, 16, 1},
     }
};

/// Java calculateMaxNumberOfVerticalSlicesAtDetailLevel: MathUtil.clamp(0, detail, len - 1).
constexpr int maxVerticalData(VerticalQuality q, int dataDetail) {
    const int last = VERTICAL_QUALITY_DETAIL_COUNT - 1;
    const int idx  = std::min(last, std::max(dataDetail, 0));
    return VERTICAL_QUALITY_SLICES[static_cast<std::size_t>(q)][static_cast<std::size_t>(idx)];
}

/// Port of EDhApiHorizontalQuality (2.0/4, 2.0/8, 2.0/12, 2.2/16, 2.2/20).
/// Java declares HIGH(2.2f, 16) into a double, i.e. the float widened (2.2000000476837158).
enum class HorizontalQuality : std::uint8_t { Lowest, Low, Medium, High, Extreme };

struct HorizontalQualityInfo {
    double quadraticBase;
    int    distanceUnitInBlocks;
};

inline constexpr std::array<HorizontalQualityInfo, 5> HORIZONTAL_QUALITY{
    {
     {2.0, 4},
     {2.0, 8},
     {2.0, 12},
     {static_cast<double>(2.2f), 16},
     {static_cast<double>(2.2f), 20},
     }
};

constexpr const HorizontalQualityInfo& info(HorizontalQuality q) {
    return HORIZONTAL_QUALITY[static_cast<std::size_t>(q)];
}

/// Port of EDhApiMaxHorizontalResolution. Ordinal == detail level (BLOCK=0 .. CHUNK=4).
enum class MaxHorizontalResolution : std::uint8_t { Block, TwoBlocks, FourBlocks, HalfChunk, Chunk };

/// Java dataPointLengthCount: 16, 8, 4, 2, 1.
constexpr int dataPointLengthCount(MaxHorizontalResolution r) {
    constexpr int t[] = {16, 8, 4, 2, 1};
    return t[static_cast<int>(r)];
}
/// Java dataPointWidth = 16 / dataPointLengthCount.
constexpr int dataPointWidth(MaxHorizontalResolution r) { return 16 / dataPointLengthCount(r); }
constexpr int detailLevel(MaxHorizontalResolution r) { return static_cast<int>(r); }

/// Java getDetailForDistance. Candidates are maxDetail and all coarser ones, coarsest first.
/// Java divides by zero (ArithmeticException) when maxDistance < candidate count; here that
/// case returns Chunk instead.
constexpr MaxHorizontalResolution detailForDistance(MaxHorizontalResolution maxDetail, int distance, int maxDistance) {
    const int count = 5 - static_cast<int>(maxDetail);
    const int step  = maxDistance / count;
    if (step == 0) {
        return MaxHorizontalResolution::Chunk;
    }
    const int index = std::min(count - 1, std::max(distance / step, 0));
    return static_cast<MaxHorizontalResolution>(4 - index);
}

/// Port of EDhApiTransparency.
enum class Transparency : std::uint8_t { Disabled, Complete };

/// Port of EDhApiGrassSideRendering.
enum class GrassSideRendering : std::uint8_t { AsGrass, FadeToDirt, AsDirt };

/// Port of EDhApiLodShading.
enum class LodShading : std::uint8_t { Auto, Enabled, Disabled };

/// Port of EDhApiBlocksToAvoid.
enum class BlocksToAvoid : std::uint8_t { None, NonColliding };
constexpr bool noCollision(BlocksToAvoid b) { return b == BlocksToAvoid::NonColliding; }

} // namespace dh
