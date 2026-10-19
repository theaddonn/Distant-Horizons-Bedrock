#pragma once

// Subset of Config.java the port needs. Each field documents "Java default / [min, max]".
// The Java GUI-only preset entries (qualityPresetSetting / threadPresetSetting) are NOT stored:
// they are derived on demand with detectQualityPreset / detectThreadPreset (Presets.h).
// Option paths ("client.graphics.quality.x") drop Java's "advanced" level.

#include <cstddef>
#include <cstdint>
#include <string>

#include "dh/config/Enums.h"
#include "dh/fulldata/Enums.h"
#include "dh/render/Enums.h"

namespace dh {

//========//
// client //
//========//

struct QualityOptions {
    /// Java Client.Advanced.Graphics.Quality. Radius of the mod's render distance in chunks. 256 / [32, 4096]
    int lodChunkRenderDistanceRadius = 256;
    /// MEDIUM
    HorizontalQuality horizontalQuality = HorizontalQuality::Medium;
    /// BLOCK
    MaxHorizontalResolution maxHorizontalResolution = MaxHorizontalResolution::Block;
    /// MEDIUM
    VerticalQuality verticalQuality = VerticalQuality::Medium;
    /// true
    bool useCameraPositionForQualityDropOff = true;
    /// true
    bool increaseQualityWhenZoomedIn = true;
    /// Detail levels zooming in can add. 4 / [1, 6]
    int maxZoomQualityIncrease = 4;
    /// COMPLETE
    Transparency transparency = Transparency::Complete;
    /// AUTO
    LodShading lodShading = LodShading::Auto;
    /// FADE_TO_DIRT
    GrassSideRendering grassSideRendering = GrassSideRendering::FadeToDirt;
    /// true
    bool ditherDhFade = true;
    /// DOUBLE_PASS
    VanillaFadeMode vanillaFadeMode = VanillaFadeMode::DoublePass;
    /// Vanilla-style biome blending radius for LODs. 3 / [0, 3]
    int lodBiomeBlending = 3;
    /// 0 = black, 1 = normal, 2 = near white. Java float 1.0f, no min/max (any finite value).
    float brightnessMultiplier = 1.0F;
    /// 0 = grey, 1 = normal, 2 = very saturated. Java float 1.0f, no min/max (any finite value).
    float saturationMultiplier = 1.0F;
};

struct CullingOptions {
    /// Java Client.Advanced.Graphics.Culling. Fraction of vanilla render distance; -1 = auto. -1.0f / [-1.0f, 1.0f]
    float overdrawPrevention = -1.0F;
    /// true
    bool reduceOverdrawWithFastMovement = true;
    /// true
    bool enableCaveCulling = true;
    /// Y where cave culling starts. 60 / [-4096, 4096]
    int caveCullingHeight = 60;
    /// true
    bool disableBeaconDistanceCulling = true;
    /// false
    bool disableFrustumCulling = false;
    /// false
    bool disableShadowPassFrustumCulling = false;
    /// Default "minecraft:barrier,minecraft:structure_void,minecraft:light,minecraft:tripwire,minecraft:brown_mushroom"
    std::string ignoredRenderBlockCsv =
        "minecraft:barrier,minecraft:structure_void,minecraft:light,minecraft:tripwire,minecraft:brown_mushroom";
    /// Default ""
    std::string ignoredRenderCaveBlockCsv;
    /// Default "minecraft:kelp,minecraft:tall_seagrass,minecraft:seagrass"
    std::string waterSubSurfaceBlockReplacementCsv = "minecraft:kelp,minecraft:tall_seagrass,minecraft:seagrass";
    /// Default "minecraft:lily_pad"
    std::string waterSurfaceBlockReplacementCsv = "minecraft:lily_pad";
    /// NON_COLLIDING
    BlocksToAvoid blocksToIgnore = BlocksToAvoid::NonColliding;
    /// true
    bool tintWithAvoidedBlocks = true;
};

struct FogOptions {
    /// Java Client.Advanced.Graphics.Fog. true
    bool enableDhFog = true;
    /// false
    bool enableVanillaFog = false;
    /// Bedrock: push the vanilla distance fog out to the LOD far distance (Java disableVanillaFog). true
    bool disableVanillaFog = true;
};

struct CloudOptions {
    /// Draw DH's own cloud layer out to the LOD far distance (Java's generic-object cloud renderer). true
    bool renderDhClouds = true;
    /// Bedrock: hide the vanilla clouds while the LOD pipeline renders (restored when it is off). true
    bool hideVanillaClouds = true;
};

struct GraphicsOptions {
    QualityOptions quality;
    CullingOptions culling;
    FogOptions     fog;
    CloudOptions   clouds;
    /// Java Client.Advanced.Graphics. true
    bool enableSsao = true;
    /// true
    bool enableAntiAliasing = true;
    /// true
    bool overrideVanillaGraphicsSettings = true;
};

struct ClientOptions {
    /// Java Client.quickEnableRendering. true
    bool            enableRendering = true;
    GraphicsOptions graphics;
};

//========//
// common //
//========//

struct WorldGeneratorOptions {
    /// Java Common.WorldGenerator. SURFACE_THEN_CHUNKS
    GeneratorPlan generatorPlan = GeneratorPlan::SurfaceThenChunks;
    /// Bedrock port safety switch: distant generation stays Disabled until scratch generation is validated in game.
    /// false
    bool allowUnvalidatedGenerator = false;
    /// Bedrock port: worker threads for scratch (distant) chunk generation. 0 = auto (max(2, cores - 2)),
    /// -1 = legacy single-thread server-thread generation. 0 / [-1, 64]
    int scratchThreads = 0;
    /// FEATURES
    DistantGeneratorMode chunkGeneratorMode = DistantGeneratorMode::Features;
    /// true
    bool limitIfServerUnhealthy = true;
    /// true
    bool pauseIfMovingQuickly = true;
    /// Java double; -1 = infinity. -1.0 / [-1.0, 1000.0]
    double surfaceRegenMaxDistancePercent = -1.0;
    /// DISABLED
    ProgressDisplayLocation showGenerationProgress = ProgressDisplayLocation::Disabled;
    /// 2 / [1, 14400]
    int generationProgressDisplayIntervalInSeconds = 2;
    /// 20 / [0, 3600]
    int generationProgressDisableMessageDisplayTimeInSeconds = 20;
    /// true
    bool generationProgressIncludeChunksPerSecond = true;
    /// 0 / [INT_MIN, INT_MAX]
    std::int32_t generationCenterChunkX = 0;
    /// 0 / [INT_MIN, INT_MAX]
    std::int32_t generationCenterChunkZ = 0;
    /// 0 disables generation bounds. 0 / [0, INT_MAX]
    std::int32_t generationMaxChunkRadius = 0;
};

struct LodBuildingOptions {
    /// Java Common.LodBuilding. false
    bool disableUnchangedChunkCheck = false;
    /// Z_STD_BLOCK. Only UNCOMPRESSED and Z_STD_BLOCK are usable; other values are reset to the default.
    DataCompressionMode dataCompression = DataCompressionMode::ZStdBlock;
    /// VISUALLY_EQUAL
    WorldCompressionMode worldCompression = WorldCompressionMode::VisuallyEqual;
};

struct MultiThreadingOptions {
    /// Java Common.MultiThreading. Default ceil(0.5 * cores) (see defaultOptions(cores)); [1, cores].
    /// The struct default below assumes 4 cores.
    int numberOfThreads = 2;
    /// 1.0 / [0.01, 1.0]
    double threadRunTimeRatio = 1.0;
    /// Java thread priority. 5 / [1, 10]
    int threadPriority = 5;
};

struct CommonOptions {
    WorldGeneratorOptions worldGenerator;
    LodBuildingOptions    lodBuilding;
    MultiThreadingOptions multiThreading;
};

//========//
// server //
//========//

struct ServerOptions {
    /// Java Server. true
    bool sendLevelKeys = true;
    /// Java default is a random int; here 0 = "not yet chosen", glue should randomize and persist. [INT_MIN, INT_MAX]
    std::int32_t serverId = 0;
    /// ""
    std::string serverKey;
    /// ""
    std::string levelKeyPrefix;
    /// 20 / [1, 100]
    int generationRequestRateLimit = 20;
    /// 4096 / [256, 4096]
    int maxGenerationRequestDistance = 4096;
    /// true
    bool enableRealTimeUpdates = true;
    /// 256 / [32, 4096]
    int realTimeUpdateDistanceRadiusInChunks = 256;
    /// true
    bool synchronizeOnLoad = true;
    /// 50 / [1, 100]
    int syncOnLoadRateLimit = 50;
    /// 4096 / [256, 4096]
    int maxSyncOnLoadRequestDistance = 4096;
    /// KB/s, 0 = unlimited. 500 / [0, 1000000]
    int playerBandwidthLimit = 500;
    /// KB/s, 0 = unlimited. 0 / [0, 10000000]
    int globalBandwidthLimit = 0;
    /// false
    bool enableAdaptiveTransferSpeed = false;
};

struct Options {
    ClientOptions client;
    CommonOptions common;
    ServerOptions server;
};

/// Java getThreadCountByPercent: clamp(1, ceil(cores * percent), cores).
int threadCountByPercent(double percent, int cores);

/// Defaults for a machine with `cores` logical processors (only numberOfThreads depends on it).
Options defaultOptions(int cores);

/// Clamp every numeric option to its [min, max], replace NaN with the default and replace
/// out-of-range enum ordinals / unsupported DataCompressionMode values with the default.
/// `cores` (> 0) is the max for multiThreading.numberOfThreads; <= 0 means "unknown": only the lower bound applies.
void clampToRange(Options& o, int cores = 0);

//==========//
// metadata //
//==========//

enum class OptionType : std::uint8_t { Bool, Int, Float, Double, String, Enum };

struct OptionMeta {
    const char* path; ///< "client.graphics.quality.lodChunkRenderDistanceRadius"
    OptionType  type;
    /// Bool: 0/1. Int/Float/Double/Enum: the value (Enum: ordinal). Unused for String.
    double             defaultValue;
    double             minValue;      ///< Enum: 0
    double             maxValue;      ///< Enum: count - 1. numberOfThreads: INT_MAX here, real max = cores
    const char*        defaultString; ///< String options only, else nullptr
    const char* const* enumNames;     ///< Enum options only (Java constant names by ordinal), else nullptr
    int                enumCount;
    bool               maxIsCoreCount; ///< numberOfThreads
    const char*        comment;        ///< short Java doc

    // Typed access into an Options object (never null). Enum options are 1-byte (std::uint8_t ordinal).
    void* (*mutablePtr)(Options&);
    const void* (*constPtr)(const Options&);
};

const OptionMeta* optionMeta();
std::size_t       optionMetaCount();
/// nullptr if unknown.
const OptionMeta* findOptionMeta(const char* path);

} // namespace dh
