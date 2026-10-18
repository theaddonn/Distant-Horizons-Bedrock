#pragma once

// Config-only enums (ports of the Java API enums under api/enums/config, quickOptions and
// worldGeneration). Enums that already exist elsewhere in the core (VerticalQuality,
// HorizontalQuality, ... in dh/render/Enums.h; WorldCompressionMode in dh/fulldata/Enums.h) are
// reused, not redefined. Ordinals match Java `ordinal()`; every enum here is a 1-byte enum so
// the option table can treat all enum-valued options uniformly.

#include <cstdint>

namespace dh {

/// EDhApiQualityPreset. Custom = "current values match no preset".
enum class QualityPreset : std::uint8_t { Custom, Minimum, Low, Medium, High, Extreme };

/// EDhApiThreadPreset.
enum class ThreadPreset : std::uint8_t { Custom, MinimalImpact, LowImpact, Balanced, Aggressive, IPaidForTheWholeCpu };

/// EDhApiMcRenderingFadeMode ("vanilla fade").
enum class VanillaFadeMode : std::uint8_t { None, SinglePass, DoublePass };

/// EDhApiGeneratorPlan.
enum class GeneratorPlan : std::uint8_t { SurfaceThenChunks, SurfaceOnly, ChunksOnly, Disabled };

constexpr bool generationEnabled(GeneratorPlan p) { return p != GeneratorPlan::Disabled; }
constexpr bool chunkGenEnabled(GeneratorPlan p) {
    return p == GeneratorPlan::SurfaceThenChunks || p == GeneratorPlan::ChunksOnly;
}
constexpr bool surfaceGenEnabled(GeneratorPlan p) {
    return p == GeneratorPlan::SurfaceThenChunks || p == GeneratorPlan::SurfaceOnly;
}

/// EDhApiDistantGeneratorMode.
enum class DistantGeneratorMode : std::uint8_t { PreExistingOnly, Surface, Features, InternalServer };

/// Java `complexity` field: 1, 4, 5, 6.
constexpr int complexity(DistantGeneratorMode m) {
    constexpr int t[] = {1, 4, 5, 6};
    return t[static_cast<int>(m)];
}

/// EDhApiDataCompressionMode. Ordinals follow Java declaration order (the persisted `value`
/// differs, see dataCompressionValue). Only Uncompressed and ZStdBlock are usable in the port.
enum class DataCompressionMode : std::uint8_t { Uncompressed, Lz4, ZStdBlock, ZStdStream, Lzma2 };

/// Java `value` field: 0, 1, 4, 2, 3 (by ordinal).
constexpr int dataCompressionValue(DataCompressionMode m) {
    constexpr int t[] = {0, 1, 4, 2, 3};
    return t[static_cast<int>(m)];
}
constexpr bool isSupported(DataCompressionMode m) {
    return m == DataCompressionMode::Uncompressed || m == DataCompressionMode::ZStdBlock;
}

/// EDhApiDistantGeneratorProgressDisplayLocation.
enum class ProgressDisplayLocation : std::uint8_t { Overlay, Chat, Log, Disabled };

} // namespace dh
