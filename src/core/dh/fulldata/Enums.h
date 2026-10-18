#pragma once

#include <cstdint>
#include <optional>

namespace dh {

/// Port of Java EDhApiWorldGenerationStep (api/enums/worldGeneration). The value is
/// persisted per column as a signed byte. NOTE: LiquidCarvers and Terrain both
/// have the value 7 (as in Java); generationStepFromValue(7) returns LiquidCarvers.
enum class GenerationStep : std::int8_t {
    DownSampled        = -1,
    Empty              = 0,
    StructureStart     = 1,
    StructureReference = 2,
    Biomes             = 3,
    Noise              = 4,
    Surface            = 5,
    Carvers            = 6,
    LiquidCarvers      = 7,
    Terrain            = 7,
    Features           = 8,
    Light              = 9,
};

/// Java EDhApiWorldGenerationStep.fromValue (first match wins); nullopt when unknown.
inline std::optional<GenerationStep> generationStepFromValue(int value) {
    constexpr GenerationStep all[] = {
        GenerationStep::DownSampled,
        GenerationStep::Empty,
        GenerationStep::StructureStart,
        GenerationStep::StructureReference,
        GenerationStep::Biomes,
        GenerationStep::Noise,
        GenerationStep::Surface,
        GenerationStep::Carvers,
        GenerationStep::LiquidCarvers,
        GenerationStep::Features,
        GenerationStep::Light,
    };
    for (GenerationStep s : all) {
        if (static_cast<int>(s) == value) return s;
    }
    return std::nullopt;
}

/// Port of Java EDhApiWorldCompressionMode (api/enums/config).
enum class WorldCompressionMode : std::int8_t {
    MergeSameBlocks = 0,
    VisuallyEqual   = 1,
};

/// Java getFromValue throws on unknown values; here nullopt.
inline std::optional<WorldCompressionMode> compressionModeFromValue(int value) {
    if (value == 0) return WorldCompressionMode::MergeSameBlocks;
    if (value == 1) return WorldCompressionMode::VisuallyEqual;
    return std::nullopt;
}

/// The four horizontal directions used by adjacent-edge data (subset of Java EDhDirection).
enum class AdjDirection : std::uint8_t { North, South, West, East };

/// Column range [minX,maxX) x [minZ,maxZ) of the edge strip adjacent in a direction.
/// Port of FullDataMinMaxPosUtil.getEncodedMinMaxPos (WIDTH = 64).
struct AdjRange {
    int minX, maxX, minZ, maxZ;
};

constexpr AdjRange adjRange(AdjDirection d, int width = 64) {
    switch (d) {
    case AdjDirection::North:
        return {0, width, 0, 1};
    case AdjDirection::South:
        return {0, width, width - 1, width};
    case AdjDirection::East:
        return {width - 1, width, 0, width};
    case AdjDirection::West:
        return {0, 1, 0, width};
    }
    return {0, 0, 0, 0};
}

} // namespace dh
