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

// Bedrock-port enums: lowercase-hyphen names (ordinal 0 = default).
constexpr const char* kVanillaFogNames[]   = {"all", "off", "distance", "fog-control"};
constexpr const char* kSkyOrderNames[]     = {"overdraw", "late", "opaque"};
constexpr const char* kLateScopeNames[]    = {"beyond-vanilla", "full"};
constexpr const char* kLightmapNames[]     = {"dynamic", "shader", "dynamic-swapped", "unlit-test", "light-debug"};
constexpr const char* kFarPlaneNames[]     = {"local-proj-push", "zfar-override", "off"};
constexpr const char* kMaterialNames[]     = {"dh", "mesh-fallback", "selection"};
constexpr const char* kDrawHookNames[]     = {"camera-after", "level-render-before", "level-render-after", "post-level-render"};

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
        DH_BOOL(
            "client.enableRendering",
            client.enableRendering,
            true,
            "Master switch: when true the LOD pipeline starts with the world, when false it stays off."
        ),
        DH_STRING(
            "client.keys.toggleRendering",
            client.keys.toggleRendering,
            "none",
            "Optional key that switches the LOD pipeline on/off while playing, e.g. \"numpad-add\" or \"f9\". "
            "\"none\" = no key. Avoid F1, F2, F3, F5, F11 (used by Minecraft)."
        ),

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
        DH_ENUM(
            "client.graphics.fog.vanillaFog",
            client.graphics.fog.vanillaFog,
            static_cast<int>(VanillaFogMode::All),
            kVanillaFogNames,
            "How Minecraft's own distance fog is pushed out to the LOD distance so DH's fog is the only one: all = "
            "both fog values, distance / fog-control = one of them, off = leave Minecraft's fog alone."
        ),
        DH_BOOL(
            "client.graphics.clouds.renderDhClouds",
            client.graphics.clouds.renderDhClouds,
            true,
            "Draw Distant Horizons' own cloud layer out to the LOD far distance."
        ),
        DH_BOOL(
            "client.graphics.clouds.backFaceCull",
            client.graphics.clouds.backFaceCull,
            true,
            "Cull the back faces of the DH cloud layer (turn off if clouds look see-through from inside)."
        ),
        DH_BOOL(
            "client.graphics.clouds.hideVanillaClouds",
            client.graphics.clouds.hideVanillaClouds,
            true,
            "Hide Minecraft's own clouds while the DH cloud layer is drawn (otherwise both overlap)."
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

        //---------- advanced / debug (Bedrock) ----------
        DH_ENUM(
            "client.advanced.skyOrder",
            client.advanced.skyOrder,
            static_cast<int>(SkyOrderMode::Overdraw),
            kSkyOrderNames,
            "Render pass the opaque LODs are drawn in. overdraw keeps stars/sun/moon behind LODs; opaque is the "
            "plain pass (stars can show through LODs); late is for troubleshooting only."
        ),
        DH_ENUM(
            "client.advanced.lateScope",
            client.advanced.lateScope,
            static_cast<int>(LateScopeMode::BeyondVanilla),
            kLateScopeNames,
            "Where the overdraw repeat is applied: beyond-vanilla = only past the vanilla render distance (cheaper), "
            "full = everywhere."
        ),
        DH_ENUM(
            "client.advanced.lightmapMode",
            client.advanced.lightmapMode,
            static_cast<int>(LightmapSource::Dynamic),
            kLightmapNames,
            "LOD lighting source: dynamic = Minecraft's lightmap, shader = built-in approximation. The rest are "
            "diagnostics."
        ),
        DH_BOOL(
            "client.advanced.nearClip",
            client.advanced.nearClip,
            true,
            "Skip LODs inside the vanilla render distance (they would only overdraw real chunks)."
        ),
        DH_ENUM(
            "client.advanced.farPlane",
            client.advanced.farPlane,
            static_cast<int>(FarPlaneMode::LocalProjPush),
            kFarPlaneNames,
            "How the camera far plane is extended so distant LODs are not clipped. zfar-override also hides "
            "the vanilla clouds; off = LODs are clipped at the vanilla far plane."
        ),
        DH_ENUM(
            "client.advanced.material",
            client.advanced.material,
            static_cast<int>(MaterialMode::Dh),
            kMaterialNames,
            "LOD material. dh = the mod's own shader (falls back automatically if it is missing)."
        ),
        DH_ENUM(
            "client.advanced.drawHook",
            client.advanced.drawHook,
            static_cast<int>(DrawHookMode::CameraAfter),
            kDrawHookNames,
            "Where in the frame the LODs are drawn. Leave on camera-after unless troubleshooting."
        ),
        DH_BOOL(
            "client.advanced.coreFrustum",
            client.advanced.coreFrustum,
            true,
            "Frustum-cull sections while building the render list (cheaper draws)."
        ),
        DH_BOOL(
            "client.advanced.drawTransparent",
            client.advanced.drawTransparent,
            true,
            "Draw the translucent LOD layer (water, ice)."
        ),
        DH_BOOL(
            "client.debug.enabled",
            client.debug.enabled,
            false,
            "Turns on the debug overlay and the numpad debug keys (see docs/DEBUG.md). Leave off for normal play."
        ),
        DH_STRING(
            "client.debug.hudKey",
            client.debug.hudKey,
            "numpad0",
            "Key that shows/hides the debug overlay (only used when debug.enabled is true)."
        ),
        DH_BOOL(
            "client.debug.logRenderPerf",
            client.debug.logRenderPerf,
            true,
            "Write a 'render: perf' summary line to dh.log every 10 s while LODs are drawn."
        ),
        DH_BOOL(
            "client.debug.logLightingDiag",
            client.debug.logLightingDiag,
            false,
            "Write a lighting diagnostics line to dh.log every 10 s (always on while debug.enabled)."
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
            true,
            "Safety switch: set to false to force distant generation off regardless of generatorPlan."
        ),
        DH_BOOL(
            "common.worldGenerator.compareMode",
            common.worldGenerator.compareMode,
            false,
            "Debug: compare generated chunks with the real ones and log the differences."
        ),
        DH_INT(
            "common.worldGenerator.scratchThreads",
            common.worldGenerator.scratchThreads,
            0,
            -1,
            64,
            "Worker threads for distant chunk generation. 0 = auto (max(2, cores - 2)); -1 = legacy "
            "single-threaded generation on the server thread."
        ),
        DH_ENUM(
            "common.worldGenerator.chunkGeneratorMode",
            common.worldGenerator.chunkGeneratorMode,
            static_cast<int>(DistantGeneratorMode::Features),
            kGenModeNames,
            "How detailed distant generation is: FEATURES = decorated terrain (trees, ores), SURFACE = base terrain only (faster)."
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
