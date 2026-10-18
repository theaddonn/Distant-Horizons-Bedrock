#pragma once

// Distant world generation seam (Java: wrapperInterfaces/worldGeneration/
// IChunkGenerator + enums EDhApiDistantGeneratorMode / EDhApiWorldGenerationStep).
//
// The core's generation queue (WP-J1/J2) asks the platform to generate a square
// of chunks and hands each finished chunk back as an IChunk for LOD building.
// Bedrock: SingleplayerGenDriver (in-process server, scratch LevelChunk) and
// BdsWorldGenerator (server DLL).
//
// Contract:
//   * queue() is non-blocking. `onChunk` is invoked once per generated chunk
//     (any thread) with a shared_ptr<IChunk>: the consumer may keep it (Java
//     kept the wrappers in a list and lit/built them after the batch; lighting
//     needs the 3x3 neighbourhood), so the chunk stays alive for as long as the
//     consumer holds the pointer. shared_ptr was chosen over a release callback
//     because the Bedrock drivers (SingleplayerGenDriver, BdsWorldGenerator)
//     just hand over the scratch-chunk wrapper they allocate anyway and need no
//     extra batch bookkeeping; drivers must not reuse/mutate a delivered chunk.
//     Then `done(ok)` exactly once: ok=false if the batch failed or was
//     cancelled (some onChunk calls may still have happened).
//   * widthChunks x widthChunks chunks starting at (minChunkX, minChunkZ).
//   * isBusy() lets the queue throttle (Java: in-progress > thread count).
//   * cancelAll() aborts pending batches; their done(false) fires before
//     cancelAll() returns or shortly after (never both nor never).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace dh {

class IChunk;

/// Java EDhApiDistantGeneratorMode. Numeric values are the Java `complexity`
/// bytes (used for ordering: higher = more complete terrain).
enum class GenerationMode : std::uint8_t {
    PreExistingOnly = 1, ///< only chunks that already exist on disk
    Surface         = 4, ///< heightmap/surface only, no features
    Features        = 5, ///< full terrain incl. features, no lighting pass
    InternalServer  = 6, ///< let the server generate and load normally
};

/// Java EDhApiWorldGenerationStep. Signed byte values are persisted per column
/// (WP-B1/D1) and compared numerically, so they must match Java exactly.
/// Note LiquidCarvers and Terrain share value 7 (Java quirk, keep).
enum class GenerationStep : std::int8_t {
    DownSampled        = -1, ///< column came from a higher-detail downsample, not from world gen
    Empty              = 0,
    StructureStart     = 1,
    StructureReference = 2,
    Biomes             = 3,
    Noise              = 4,
    Surface            = 5,
    Carvers            = 6,
    LiquidCarvers      = 7,
    Terrain            = 7, ///< alias of LiquidCarvers
    Features           = 8,
    Light              = 9,
};

/// Java EDhApiWorldGenerationStep.name. Value 7 reports "liquid_carvers"
/// (Java fromValue returns the first declared match).
constexpr const char* generationStepName(GenerationStep step) {
    switch (static_cast<std::int8_t>(step)) {
    case -1:
        return "down_sampled";
    case 0:
        return "empty";
    case 1:
        return "structure_start";
    case 2:
        return "structure_reference";
    case 3:
        return "biomes";
    case 4:
        return "noise";
    case 5:
        return "surface";
    case 6:
        return "carvers";
    case 7:
        return "liquid_carvers";
    case 8:
        return "features";
    case 9:
        return "light";
    default:
        return "?";
    }
}

/// Java fromValue: nullopt for values outside -1..9.
constexpr std::optional<GenerationStep> generationStepFromValue(int value) {
    if (value < -1 || value > 9) return std::nullopt;
    return static_cast<GenerationStep>(value);
}

/// Java fromName: accepts both "liquid_carvers" and "terrain" (-> value 7).
constexpr std::optional<GenerationStep> generationStepFromName(std::string_view name) {
    for (int v = -1; v <= 9; ++v) {
        if (name == generationStepName(static_cast<GenerationStep>(v))) return static_cast<GenerationStep>(v);
    }
    if (name == "terrain") return GenerationStep::Terrain;
    return std::nullopt;
}

struct IChunkGenerator {
    using ChunkCallback = std::function<void(std::shared_ptr<IChunk>)>;
    using DoneCallback  = std::function<void(bool ok)>;

    virtual ~IChunkGenerator() = default;

    virtual void queue(
        std::int32_t   minChunkX,
        std::int32_t   minChunkZ,
        std::int32_t   widthChunks,
        GenerationMode mode,
        GenerationStep targetStep,
        ChunkCallback  onChunk,
        DoneCallback   done
    ) = 0;

    virtual bool isBusy() const = 0;
    virtual void cancelAll()    = 0;
};

} // namespace dh
