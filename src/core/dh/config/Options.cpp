// Option metadata table, defaults and clamping. The table is the single source of truth for
// JSON (ConfigStore) and for a future UI; the struct member initializers in Options.h must agree
// with it (checked by tests/unit/config_test.cpp).

#include "dh/config/Options.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>

namespace dh {

namespace {

constexpr const char* kVerticalQualityNames[] =
    {"HEIGHT_MAP", "LOW", "MEDIUM", "HIGH", "VERY_HIGH", "EXTREME", "PIXEL_ART"};
constexpr const char* kHorizontalQualityNames[] = {"LOWEST", "LOW", "MEDIUM", "HIGH", "EXTREME"};
constexpr const char* kMaxResolutionNames[]     = {"BLOCK", "TWO_BLOCKS", "FOUR_BLOCKS", "HALF_CHUNK", "CHUNK"};
constexpr const char* kTransparencyNames[]      = {"DISABLED", "COMPLETE"};
constexpr const char* kGrassNames[]             = {"AS_GRASS", "FADE_TO_DIRT", "AS_DIRT"};
constexpr const char* kLodShadingNames[]        = {"AUTO", "ENABLED", "DISABLED"};
constexpr const char* kBlocksToAvoidNames[]     = {"NONE", "NON_COLLIDING"};
constexpr const char* kFadeNames[]              = {"NONE", "SINGLE_PASS", "DOUBLE_PASS"};
constexpr const char* kPlanNames[]              = {"SURFACE_THEN_CHUNKS", "SURFACE_ONLY", "CHUNKS_ONLY", "DISABLED"};
constexpr const char* kGenModeNames[]           = {"PRE_EXISTING_ONLY", "SURFACE", "FEATURES", "INTERNAL_SERVER"};
constexpr const char* kProgressNames[]          = {"OVERLAY", "CHAT", "LOG", "DISABLED"};
constexpr const char* kDataCompressionNames[]   = {"UNCOMPRESSED", "LZ4", "Z_STD_BLOCK", "Z_STD_STREAM", "LZMA2"};
constexpr const char* kWorldCompressionNames[]  = {"MERGE_SAME_BLOCKS", "VISUALLY_EQUAL"};

#define DH_PTRS(expr)                                                                                                  \
    +[](Options& o) -> void* { return &o.expr; }, +[](const Options& o) -> const void* { return &o.expr; }

#define DH_BOOL(path, expr, def, doc)                                                                                  \
    OptionMeta { path, OptionType::Bool, (def) ? 1.0 : 0.0, 0.0, 1.0, nullptr, nullptr, 0, false, doc, DH_PTRS(expr) }
#define DH_INT(path, expr, def, mn, mx, doc)                                                                           \
    OptionMeta { path, OptionType::Int, def, mn, mx, nullptr, nullptr, 0, false, doc, DH_PTRS(expr) }
#define DH_FLOAT(path, expr, def, mn, mx, doc)                                                                         \
    OptionMeta { path, OptionType::Float, def, mn, mx, nullptr, nullptr, 0, false, doc, DH_PTRS(expr) }
#define DH_DOUBLE(path, expr, def, mn, mx, doc)                                                                        \
    OptionMeta { path, OptionType::Double, def, mn, mx, nullptr, nullptr, 0, false, doc, DH_PTRS(expr) }
#define DH_STRING(path, expr, def, doc)                                                                                \
    OptionMeta { path, OptionType::String, 0.0, 0.0, 0.0, def, nullptr, 0, false, doc, DH_PTRS(expr) }
#define DH_ENUM(path, expr, def, names, doc)                                                                           \
    OptionMeta {                                                                                                       \
        path, OptionType::Enum, static_cast<double>(def), 0.0, static_cast<double>(std::size(names) - 1), nullptr,     \
            names, static_cast<int>(std::size(names)), false, doc, DH_PTRS(expr)                                       \
    }

constexpr double kIntMin = INT_MIN;
constexpr double kIntMax = INT_MAX;

struct Table {
    const OptionMeta* data;
    std::size_t       size;
};

Table table() {
    static const OptionMeta kTable[] = {
        //---------- client ----------
        DH_BOOL("client.enableRendering", client.enableRendering, true, "Master switch for LOD rendering."),

        DH_INT(
            "client.graphics.quality.lodChunkRenderDistanceRadius",
            client.graphics.quality.lodChunkRenderDistanceRadius,
            256,
            32,
            4096,
            "Radius of the mod's render distance, in chunks (best effort)."
        ),
        DH_ENUM(
            "client.graphics.quality.horizontalQuality",
            client.graphics.quality.horizontalQuality,
            static_cast<int>(HorizontalQuality::Medium),
            kHorizontalQualityNames,
            "How far apart drops in LOD quality are."
        ),
        DH_ENUM(
            "client.graphics.quality.maxHorizontalResolution",
            client.graphics.quality.maxHorizontalResolution,
            static_cast<int>(MaxHorizontalResolution::Block),
            kMaxResolutionNames,
            "Maximum detail LODs can render at."
        ),
        DH_ENUM(
            "client.graphics.quality.verticalQuality",
            client.graphics.quality.verticalQuality,
            static_cast<int>(VerticalQuality::Medium),
            kVerticalQualityNames,
            "How well LODs represent overhangs, caves, etc."
        ),
        DH_BOOL(
            "client.graphics.quality.useCameraPositionForQualityDropOff",
            client.graphics.quality.useCameraPositionForQualityDropOff,
            true,
            "Use the camera (not the player) position for quality drop-off."
        ),
        DH_BOOL(
            "client.graphics.quality.increaseQualityWhenZoomedIn",
            client.graphics.quality.increaseQualityWhenZoomedIn,
            true,
            "Increase LOD quality when the camera is zoomed in."
        ),
        DH_INT(
            "client.graphics.quality.maxZoomQualityIncrease",
            client.graphics.quality.maxZoomQualityIncrease,
            4,
            1,
            6,
            "How many detail levels zooming in can increase LOD quality by."
        ),
        DH_ENUM(
            "client.graphics.quality.transparency",
            client.graphics.quality.transparency,
            static_cast<int>(Transparency::Complete),
            kTransparencyNames,
            "How LOD transparency is handled."
        ),
        DH_ENUM(
            "client.graphics.quality.lodShading",
            client.graphics.quality.lodShading,
            static_cast<int>(LodShading::Auto),
            kLodShadingNames,
            "How LODs are shaded."
        ),
        DH_ENUM(
            "client.graphics.quality.grassSideRendering",
            client.graphics.quality.grassSideRendering,
            static_cast<int>(GrassSideRendering::FadeToDirt),
            kGrassNames,
            "How the sides/bottom of grass LODs render."
        ),
        DH_BOOL(
            "client.graphics.quality.ditherDhFade",
            client.graphics.quality.ditherDhFade,
            true,
            "LODs fade away as you get closer to them."
        ),
        DH_ENUM(
            "client.graphics.quality.vanillaFadeMode",
            client.graphics.quality.vanillaFadeMode,
            static_cast<int>(VanillaFadeMode::DoublePass),
            kFadeNames,
            "How vanilla Minecraft fades into LODs."
        ),
        DH_INT(
            "client.graphics.quality.lodBiomeBlending",
            client.graphics.quality.lodBiomeBlending,
            3,
            0,
            3,
            "Biome blending radius for LODs (0 = off)."
        ),
        DH_FLOAT(
            "client.graphics.quality.brightnessMultiplier",
            client.graphics.quality.brightnessMultiplier,
            1.0,
            -FLT_MAX,
            FLT_MAX,
            "How bright LOD colors are (1 = normal). No Java range."
        ),
        DH_FLOAT(
            "client.graphics.quality.saturationMultiplier",
            client.graphics.quality.saturationMultiplier,
            1.0,
            -FLT_MAX,
            FLT_MAX,
            "How saturated LOD colors are (1 = normal). No Java range."
        ),

        DH_BOOL(
            "client.graphics.enableSsao",
            client.graphics.enableSsao,
            true,
            "Enable Screen Space Ambient Occlusion."
        ),
        DH_BOOL(
            "client.graphics.enableAntiAliasing",
            client.graphics.enableAntiAliasing,
            true,
            "Enable anti-aliasing."
        ),
        DH_BOOL(
            "client.graphics.overrideVanillaGraphicsSettings",
            client.graphics.overrideVanillaGraphicsSettings,
            true,
            "Automatically change some vanilla graphics settings (clouds, chunk fading) for a better experience."
        ),

        DH_BOOL("client.graphics.fog.enableDhFog", client.graphics.fog.enableDhFog, true, "Draw fog on DH LODs."),
        DH_BOOL(
            "client.graphics.fog.enableVanillaFog",
            client.graphics.fog.enableVanillaFog,
            false,
            "Should Minecraft's fog render."
        ),

        DH_FLOAT(
            "client.graphics.culling.overdrawPrevention",
            client.graphics.culling.overdrawPrevention,
            -1.0,
            -1.0,
            1.0,
            "Start distance as a fraction of vanilla render distance; -1 = auto."
        ),
        DH_BOOL(
            "client.graphics.culling.reduceOverdrawWithFastMovement",
            client.graphics.culling.reduceOverdrawWithFastMovement,
            true,
            "Shrink the overdraw radius when moving quickly."
        ),
        DH_BOOL(
            "client.graphics.culling.enableCaveCulling",
            client.graphics.culling.enableCaveCulling,
            true,
            "Don't render caves."
        ),
        DH_INT(
            "client.graphics.culling.caveCullingHeight",
            client.graphics.culling.caveCullingHeight,
            60,
            -4096,
            4096,
            "Y value where cave culling starts."
        ),
        DH_BOOL(
            "client.graphics.culling.disableBeaconDistanceCulling",
            client.graphics.culling.disableBeaconDistanceCulling,
            true,
            "If true all beacons are rendered."
        ),
        DH_BOOL(
            "client.graphics.culling.disableFrustumCulling",
            client.graphics.culling.disableFrustumCulling,
            false,
            "Draw LODs outside the camera frustum too."
        ),
        DH_BOOL(
            "client.graphics.culling.disableShadowPassFrustumCulling",
            client.graphics.culling.disableShadowPassFrustumCulling,
            false,
            "Same as frustum culling, for shadow passes."
        ),
        DH_STRING(
            "client.graphics.culling.ignoredRenderBlockCsv",
            client.graphics.culling.ignoredRenderBlockCsv,
            "minecraft:barrier,minecraft:structure_void,minecraft:light,minecraft:tripwire,minecraft:brown_mushroom",
            "Comma separated block ids that are never rendered."
        ),
        DH_STRING(
            "client.graphics.culling.ignoredRenderCaveBlockCsv",
            client.graphics.culling.ignoredRenderCaveBlockCsv,
            "",
            "Block ids not rendered in 0 sky light underground areas."
        ),
        DH_STRING(
            "client.graphics.culling.waterSubSurfaceBlockReplacementCsv",
            client.graphics.culling.waterSubSurfaceBlockReplacementCsv,
            "minecraft:kelp,minecraft:tall_seagrass,minecraft:seagrass",
            "Block ids replaced by water when visible on the water surface."
        ),
        DH_STRING(
            "client.graphics.culling.waterSurfaceBlockReplacementCsv",
            client.graphics.culling.waterSurfaceBlockReplacementCsv,
            "minecraft:lily_pad",
            "Block ids removed when on top of water."
        ),
        DH_ENUM(
            "client.graphics.culling.blocksToIgnore",
            client.graphics.culling.blocksToIgnore,
            static_cast<int>(BlocksToAvoid::NonColliding),
            kBlocksToAvoidNames,
            "Which blocks are rendered as LODs."
        ),
        DH_BOOL(
            "client.graphics.culling.tintWithAvoidedBlocks",
            client.graphics.culling.tintWithAvoidedBlocks,
            true,
            "Blocks below avoided blocks gain the avoided block's color."
        ),

        //---------- common ----------
        DH_ENUM(
            "common.worldGenerator.generatorPlan",
            common.worldGenerator.generatorPlan,
            static_cast<int>(GeneratorPlan::SurfaceThenChunks),
            kPlanNames,
            "How LODs are generated outside the vanilla render distance."
        ),
        DH_BOOL(
            "common.worldGenerator.allowUnvalidatedGenerator",
            common.worldGenerator.allowUnvalidatedGenerator,
            false,
            "Bedrock port: allow distant generation (generatorPlan) although scratch chunk generation is not yet "
            "validated in game."
        ),
        DH_INT(
            "common.worldGenerator.scratchThreads",
            common.worldGenerator.scratchThreads,
            0,
            -1,
            64,
            "Bedrock port: worker threads for distant chunk generation. 0 = auto (max(2, cores - 2)); -1 = legacy "
            "single-threaded generation on the server thread."
        ),
        DH_ENUM(
            "common.worldGenerator.chunkGeneratorMode",
            common.worldGenerator.chunkGeneratorMode,
            static_cast<int>(DistantGeneratorMode::Features),
            kGenModeNames,
            "How detailed distant generation is."
        ),
        DH_BOOL(
            "common.worldGenerator.limitIfServerUnhealthy",
            common.worldGenerator.limitIfServerUnhealthy,
            true,
            "Pause world gen when the server thread is slow."
        ),
        DH_BOOL(
            "common.worldGenerator.pauseIfMovingQuickly",
            common.worldGenerator.pauseIfMovingQuickly,
            true,
            "Pause world gen when moving quickly."
        ),
        DH_DOUBLE(
            "common.worldGenerator.surfaceRegenMaxDistancePercent",
            common.worldGenerator.surfaceRegenMaxDistancePercent,
            -1.0,
            -1.0,
            1000.0,
            "Limit of the second (chunk) pass as a fraction of render distance; -1 = infinity."
        ),
        DH_ENUM(
            "common.worldGenerator.showGenerationProgress",
            common.worldGenerator.showGenerationProgress,
            static_cast<int>(ProgressDisplayLocation::Disabled),
            kProgressNames,
            "Where generator progress is displayed."
        ),
        DH_INT(
            "common.worldGenerator.generationProgressDisplayIntervalInSeconds",
            common.worldGenerator.generationProgressDisplayIntervalInSeconds,
            2,
            1,
            14400,
            "How often generator progress is displayed."
        ),
        DH_INT(
            "common.worldGenerator.generationProgressDisableMessageDisplayTimeInSeconds",
            common.worldGenerator.generationProgressDisableMessageDisplayTimeInSeconds,
            20,
            0,
            3600,
            "How long the disable-instructions are shown (0 = hidden)."
        ),
        DH_BOOL(
            "common.worldGenerator.generationProgressIncludeChunksPerSecond",
            common.worldGenerator.generationProgressIncludeChunksPerSecond,
            true,
            "Include chunks/s in the progress output."
        ),
        DH_INT(
            "common.worldGenerator.generationCenterChunkX",
            common.worldGenerator.generationCenterChunkX,
            0,
            kIntMin,
            kIntMax,
            "Center X chunk of the generation bounds."
        ),
        DH_INT(
            "common.worldGenerator.generationCenterChunkZ",
            common.worldGenerator.generationCenterChunkZ,
            0,
            kIntMin,
            kIntMax,
            "Center Z chunk of the generation bounds."
        ),
        DH_INT(
            "common.worldGenerator.generationMaxChunkRadius",
            common.worldGenerator.generationMaxChunkRadius,
            0,
            0,
            kIntMax,
            "Max radius in chunks where generation is allowed (0 = bounds disabled)."
        ),

        DH_BOOL(
            "common.lodBuilding.disableUnchangedChunkCheck",
            common.lodBuilding.disableUnchangedChunkCheck,
            false,
            "Don't skip chunks whose hash is unchanged."
        ),
        DH_ENUM(
            "common.lodBuilding.dataCompression",
            common.lodBuilding.dataCompression,
            static_cast<int>(DataCompressionMode::ZStdBlock),
            kDataCompressionNames,
            "Stored data compression. Only UNCOMPRESSED and Z_STD_BLOCK are supported."
        ),
        DH_ENUM(
            "common.lodBuilding.worldCompression",
            common.lodBuilding.worldCompression,
            static_cast<int>(WorldCompressionMode::VisuallyEqual),
            kWorldCompressionNames,
            "How block data is compressed when creating LOD data."
        ),

        OptionMeta{
                   "common.multiThreading.numberOfThreads", OptionType::Int,
                   2.0, 1.0,
                   kIntMax, nullptr,
                   nullptr, 0,
                   true, "Threads used by DH: default ceil(0.5 * cores), max cores.",
                   DH_PTRS(common.multiThreading.numberOfThreads)
        },
        DH_DOUBLE(
            "common.multiThreading.threadRunTimeRatio",
            common.multiThreading.threadRunTimeRatio,
            1.0,
            0.01,
            1.0,
            "Fraction of time each thread may run before idling."
        ),
        DH_INT(
            "common.multiThreading.threadPriority",
            common.multiThreading.threadPriority,
            5,
            1,
            10,
            "Thread priority of the primary pools."
        ),

        //---------- server ----------
        DH_BOOL("server.sendLevelKeys", server.sendLevelKeys, true, "Server sends level keys for each world."),
        DH_INT("server.serverId", server.serverId, 0, kIntMin, kIntMax, "Autogenerated id (0 = not yet chosen)."),
        DH_STRING("server.serverKey", server.serverKey, "", "Custom server key for reusing the same LOD folder."),
        DH_STRING("server.levelKeyPrefix", server.levelKeyPrefix, "", "Prefix of the level keys sent to clients."),
        DH_INT(
            "server.generationRequestRateLimit",
            server.generationRequestRateLimit,
            20,
            1,
            100,
            "LOD generation requests per second a client may send."
        ),
        DH_INT(
            "server.maxGenerationRequestDistance",
            server.maxGenerationRequestDistance,
            4096,
            256,
            4096,
            "Distance allowed to generate around the player."
        ),
        DH_BOOL(
            "server.enableRealTimeUpdates",
            server.enableRealTimeUpdates,
            true,
            "Clients receive real-time LOD updates."
        ),
        DH_INT(
            "server.realTimeUpdateDistanceRadiusInChunks",
            server.realTimeUpdateDistanceRadiusInChunks,
            256,
            32,
            4096,
            "Distance around the player that receives updates."
        ),
        DH_BOOL(
            "server.synchronizeOnLoad",
            server.synchronizeOnLoad,
            true,
            "Clients receive updated LODs when loading."
        ),
        DH_INT(
            "server.syncOnLoadRateLimit",
            server.syncOnLoadRateLimit,
            50,
            1,
            100,
            "LOD sync requests per second a client may send."
        ),
        DH_INT(
            "server.maxSyncOnLoadRequestDistance",
            server.maxSyncOnLoadRequestDistance,
            4096,
            256,
            4096,
            "Distance allowed to be synchronized around the player."
        ),
        DH_INT(
            "server.playerBandwidthLimit",
            server.playerBandwidthLimit,
            500,
            0,
            1000000,
            "Per-player upload limit in KB/s (0 = unlimited)."
        ),
        DH_INT(
            "server.globalBandwidthLimit",
            server.globalBandwidthLimit,
            0,
            0,
            10000000,
            "Global upload limit in KB/s (0 = unlimited)."
        ),
        DH_BOOL(
            "server.enableAdaptiveTransferSpeed",
            server.enableAdaptiveTransferSpeed,
            false,
            "Adapt transfer rate to client performance."
        ),
    };
    return {kTable, std::size(kTable)};
}

template <class T>
T clampNum(double v, double def, double mn, double mx) {
    if (std::isnan(v)) v = def;
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    return static_cast<T>(v);
}

} // namespace

const OptionMeta* optionMeta() { return table().data; }

std::size_t optionMetaCount() { return table().size; }

const OptionMeta* findOptionMeta(const char* path) {
    if (path == nullptr) return nullptr;
    const Table tb = table();
    for (std::size_t i = 0; i < tb.size; ++i) {
        if (std::strcmp(tb.data[i].path, path) == 0) return &tb.data[i];
    }
    return nullptr;
}

int threadCountByPercent(double percent, int cores) {
    if (cores < 1) cores = 1;
    int count = static_cast<int>(std::ceil(static_cast<double>(cores) * percent));
    if (count < 1) count = 1;
    if (count > cores) count = cores;
    return count;
}

Options defaultOptions(int cores) {
    Options o;
    o.common.multiThreading.numberOfThreads = threadCountByPercent(0.5, cores);
    return o;
}

void clampToRange(Options& o, int cores) {
    const Table tb = table();
    for (std::size_t i = 0; i < tb.size; ++i) {
        const OptionMeta& m = tb.data[i];
        void*             p = m.mutablePtr(o);
        switch (m.type) {
        case OptionType::Int: {
            double mx = m.maxValue;
            if (m.maxIsCoreCount && cores > 0) mx = cores;
            *static_cast<std::int32_t*>(p) = clampNum<std::int32_t>(
                static_cast<double>(*static_cast<std::int32_t*>(p)),
                m.defaultValue,
                m.minValue,
                mx
            );
            break;
        }
        case OptionType::Float:
            *static_cast<float*>(p) =
                clampNum<float>(static_cast<double>(*static_cast<float*>(p)), m.defaultValue, m.minValue, m.maxValue);
            break;
        case OptionType::Double:
            *static_cast<double*>(p) =
                clampNum<double>(*static_cast<double*>(p), m.defaultValue, m.minValue, m.maxValue);
            break;
        case OptionType::Enum: {
            auto& e = *static_cast<std::uint8_t*>(p);
            if (e >= m.enumCount) e = static_cast<std::uint8_t>(m.defaultValue);
            break;
        }
        case OptionType::Bool:
        case OptionType::String:
            break;
        }
    }
    if (!isSupported(o.common.lodBuilding.dataCompression)) {
        o.common.lodBuilding.dataCompression = DataCompressionMode::ZStdBlock;
    }
    // Presets/other code relies on this invariant even without a known core count.
    if (o.common.multiThreading.numberOfThreads < 1) o.common.multiThreading.numberOfThreads = 1;
}

} // namespace dh
