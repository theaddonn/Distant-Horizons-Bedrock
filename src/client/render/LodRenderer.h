#pragma once

// Stage-A LOD renderer: owns the MeshUploader (the IVertexSink the core uploads into), the draw settings and the
// per-frame draw loop (budgeted upload pump, frustum + near cut-off culling, opaque then transparent draws with a
// per-section world-matrix translate). Render thread only, except sink() which any thread may use.
// All the unknowns R0-R3 are toggles in Config (see the internal stage-a-render spike notes).

#include "client/render/CloudRenderer.h"
#include "client/render/MeshUploader.h"

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dh {
struct Options;
}
class BaseActorRenderContext;
class IClientInstance;
class ScreenContext;
struct ViewRenderObject;

namespace dh::render {

enum class MaterialChoice : int { Selection = 0, MeshFallback = 1, Dh = 2 };
enum class OriginMode : int { CameraRelative = 0, Absolute = 1 };
enum class FarMode : int { Off = 0, LocalProjPush = 1, ZFarOverride = 2 };
/// DH_Terrain lighting path (client.advanced.lightmapMode). Run 13 (2026-10-19): the earlier "black" results (dynamic texture,
/// procedural shader) are explained by the missing uv0 vertex field (MeshUploader); pending in-game confirmation.
enum class LightmapMode : int {
    Shader  = 0, ///< no texture: procedural Java lightmap in the FS (packed light + skyDarken) [default]
    Dynamic = 1, ///< vanilla lightmap: dynamic texture BrightnessTexture<subClientId> at slot 0 (u = block, v = sky)
    DynamicSwapped = 2, ///< same, lookup axes swapped (u = sky, v = block)
    UnlitTest      = 3, ///< u_model[3][3] = 2: the FS keeps the raw vertex colour (flag-path self test)
    LightDebug     = 4, ///< u_model[3][3] = -3: the FS paints R = sky/15, G = block/15, B = 0.25 (1.0: uv0 missing)
};
inline constexpr int         kLightmapModeCount = 5;
inline constexpr char const* kLightmapModeNames[] =
    {"shader", "dynamic", "dynamic-swapped", "unlit-test", "light-debug"};

/// Where the LOD draw is hooked (client.advanced.drawHook).
enum class DrawHook : int { CameraAfter = 0, LevelRenderBefore = 1, LevelRenderAfter = 2, PostLevelRender = 3 };
inline constexpr int kDrawHookCount = 4;

/// Which dragon frame-graph pass the opaque LOD layer lands in (client.advanced.skyOrder; Run 13). RenderDragon bins
/// every draw by material pass kind and runs Opaque -> AlphaTest -> Transparent; Stars / SunMoon are Transparent-only,
/// so they are drawn after (and depth-test in front of) opaque LODs whatever the hook. `Late` draws the opaque layer
/// with DH_TerrainLate (Blending flag, One/Zero factors, depth write) so it lands in the Transparent pass after the
/// sky inserts; `Overdraw` keeps the opaque draw and repeats it with DH_TerrainLate (2x draw calls); `Opaque` is the
/// pre-Run-13 behaviour (stars show through LODs).
/// Run 14: the Transparent pass sorts its draws back to front by an unstable per-draw distance key with no submission
/// tie-break (static RE, the internal porting notes Run 14), so in `Late` a section's opaque draw can land after its own ice or
/// water and overwrite it (black flicker). `Overdraw` is therefore the default: the sea floor is already in the depth
/// buffer from the Opaque pass, and the translucent layer is drawn with DH_TerrainWater (blend + depth write), so a
/// late repeat ordered after the water fails the depth test. `Late` keeps DH_TerrainTransparent (A/B repro only).
enum class SkyOrder : int { Late = 0, Overdraw = 1, Opaque = 2 };
inline constexpr int kSkyOrderCount = 3;

/// Run 15: which fragments the Overdraw late repeat (DH_TerrainLate in the Transparent pass) keeps. `BeyondVanilla`
/// (default) hard-clips it at the vanilla render distance (fadeEnd) and skips sections lying entirely inside: there
/// every LOD fragment is nearer than the star sphere, so the stars already fail the depth test, and the repeat would
/// only paint the LOD seabed over vanilla translucent water in the 115..144 overlap band. `Full` is the Run 14
/// behaviour (same near clip / dither as the Opaque draw). client.advanced.lateScope.
enum class LateScope : int { BeyondVanilla = 0, Full = 1 };

/// A section that passed the cull this frame (translate = draw-space min corner, dist2 = horizontal centre distance²,
/// maxDist = 3D distance of its farthest AABB corner from the camera).
struct VisibleSection {
    MeshUploader::Section const* sec = nullptr;
    glm::vec3                    t{0.0f};
    float                        dist2   = 0.0f;
    float                        maxDist = 0.0f;
};

/// The per-frame view data the draw needs, copied from ViewRenderObject::mViewData.
struct ViewSnapshot {
    bool       isShadowPass = false;
    glm::dvec3 cameraPos{0.0};
    float      renderDistance = 0.0f;
    float      skyBrightness  = 1.0f; ///< ViewRenderData::mSkyBrightnessScalar (daylight factor for shader lighting)
    glm::vec3  skyColor{0.5f, 0.7f, 1.0f}; ///< ViewRenderData::mSkyColor (cloud tint)
    bool       valid = false;
};

struct LodRendererConfig {
    DrawHook drawHook = DrawHook::CameraAfter;
    SkyOrder skyOrder = SkyOrder::Overdraw; // Run 14 (was Late)
    LateScope lateScope = LateScope::BeyondVanilla; // Run 15
    /// selectionOpaqueMaterial drops the green channel (magenta terrain, 2026-10-19); the fogged fallback is the
    /// Stage-A default (client.advanced.material).
    MaterialChoice material   = MaterialChoice::Dh; // -> MeshFallback (fogged) -> selection, whichever resolves
    OriginMode     origin     = OriginMode::CameraRelative; ///< fixed (not an option)
    FarMode        farMode    = FarMode::LocalProjPush; // R3 (2026-10-19): zfar-override makes the clouds vanish
    float          farBlocks  = 20000.0f;               ///< far plane used by LocalProjPush / ZFarOverride

    std::size_t maxUploadsPerFrame = 8;
    bool        frustumCull        = true;
    bool        drawTransparent    = true;
    bool        coreFrustum        = true; ///< cull in the core RenderList (client.advanced.coreFrustum; off: only Frustum5)
    /// Draw every uploaded section instead of the world pipeline's render list (set while the DH_TEST_FEED test feed
    /// is active, whose sections are not part of any quad tree).
    bool drawAllUploaded = false;

    /// Near cut-off: skip a section whose horizontal footprint lies entirely within
    /// vanillaRenderDistanceBlocks * overdraw of the camera. vanillaRenderDistanceBlocks <= 0: take it from
    /// ViewRenderData::mRenderDistance (values < 64 are treated as chunks). Off by default during bring-up so the test
    /// feed (placed around the player) is visible.
    bool  nearCutoff                  = true; ///< applies only while the world pipeline supplies the list
    float vanillaRenderDistanceBlocks = 0.0f;
    float overdraw                    = 1.0f;
    float fogStartFraction            = 0.6f; ///< DH_Terrain far fog starts at this fraction of the LOD far distance

    float        skyBrightness    = 1.0f;  ///< CPU light stand-in input (0 night .. 1 day); fallback materials only
    bool         lightmapSwapAxes = false; ///< DH_Terrain: swap the lightmap lookup axes (default u = block, v = sky)
    LightmapMode lightmapMode     = LightmapMode::Dynamic; ///< vanilla lightmap (verified in game 2026-10-19), shader formula as fallback
    float        minYBlocks       = -64.0f;               ///< DH y 0 maps to this Bedrock y
};

class LodRenderer {
public:
    LodRenderer();

    /// The sink the core pipeline (G2) uploads into.
    MeshUploader&       sink() { return uploader_; }
    MeshUploader const& sink() const { return uploader_; }

    LodRendererConfig& config() { return cfg_; }

    /// Re-applies the conversion-related config (primitive, light, y offset) to the uploader.
    void applyMeshSettings();

    /// Called from the LevelRendererCamera::$render hook after the vanilla world has been drawn.
    void renderFrame(BaseActorRenderContext& rc, ViewRenderObject const& vro, IClientInstance& ci);

    /// Drawing core (any hook point).
    void drawFrame(ScreenContext& ctx, ViewSnapshot const& view, IClientInstance& ci);
    /// Stores the latest view snapshot (render thread; called from the camera hook every frame).
    void                noteView(ViewRenderObject const& vro);
    ViewSnapshot const& lastView() const { return lastView_; }
    static ViewSnapshot snapshotOf(ViewRenderObject const& vro);
    void                cycleDrawHook();
    char const*         drawHookName() const;

    /// Last camera position seen by renderFrame (block coordinates); false before the first frame.
    bool lastCameraPos(glm::dvec3& out) const;
    /// Records the camera position from a frame the renderer did not draw (renderer off), so the test feed can be
    /// placed around the player before anything has been rendered.
    void observeCamera(ViewRenderObject const& vro);

    /// Fills the render config from the user options (call once at install; a restart applies changes).
    void applyOptions(dh::Options const& o);

    /// Debug-key helpers (client.debug.enabled; they log the new value, nothing is saved).
    void cycleMaterial();
    void cycleFarMode();
    void toggleNearCutoff();
    /// Cycle SkyOrder (which pass the opaque LOD layer is drawn in).
    void        cycleSkyOrder();
    char const* skyOrderName() const;
    /// Toggle LateScope (Overdraw late-repeat coverage).
    void        toggleLateScope();
    char const* lateScopeName() const;
    /// Cycle the lightmap source / binding path (LightmapMode).
    void cycleLightmapMode();
    /// DH clouds on/off (the vanilla clouds come back while off).
    void toggleClouds();

    // ---- clouds (CloudRenderer) ----
    /// Effective switch: option client.graphics.clouds.renderDhClouds && the runtime debug toggle.
    bool cloudsWanted() const;
    /// Camera hook: the dimension's cloud height (LevelRendererCamera::mCloudHeight).
    void noteCloudHeight(float y) { cloudHeight_ = y; }
    /// Hooks: a non-shadow view had ViewRenderData::mDrawClouds set by the game (before we cleared it). DH clouds are
    /// only drawn while vanilla would draw its own (cloud setting on, overworld, ...).
    void noteVanillaClouds();
    /// True when the DH cloud layer was drawn within the last quarter second (the hand-off for hiding vanilla's).
    bool                 cloudsRecentlyDrawn() const;
    CloudRenderer const& clouds() const { return clouds_; }

    char const* materialName() const;
    char const* farModeName() const;

private:
    LodRendererConfig cfg_;
    ViewSnapshot      lastView_;
    MeshUploader      uploader_;
    CloudRenderer     clouds_{uploader_};

    bool                      cloudsToggle_    = true;
    bool                      logPerf_         = true;
    bool                      logLightingDiag_ = false;
    std::atomic<float>        cloudHeight_{192.0f};
    std::atomic<std::int64_t> vanillaCloudsSeenNs_{0}; ///< steady_clock ns of the last mDrawClouds==true
    std::atomic<std::int64_t> cloudsDrawnNs_{0};
    bool                      loggedCloudTint_ = false;

    std::atomic<bool>                     haveCam_{false};
    std::atomic<double>                   camX_{0}, camY_{0}, camZ_{0};
    double                                emaMs_           = 0;
    bool                                  loggedFirstDraw_ = false;
    glm::dvec3                            lastCamPos_{0};
    std::chrono::steady_clock::time_point lastCamTime_{};
    bool                                  haveLastCam_            = false;
    double                                speedEma_               = 0.0;
    bool                                  loggedCutoff_           = false;
    bool                                  warnedDh_               = false;
    bool                                  warnedMaterialFallback_ = false;
    bool                                  loggedDhMaterial_       = false;
    bool                                  loggedTransMaterial_    = false;
    bool                                  loggedSkyOrder_         = false;
    bool                                  warnedNoLightmap_       = false;
    bool                                  loggedLightmapSize_     = false;
    bool cpuLighting_      = false; ///< ConvertParams::cpuLighting currently applied (DH_Terrain default: off)
    bool lightSwapApplied_ = false;
    std::chrono::steady_clock::time_point lastLightDiag_{};

    // Run 15 perf accounting (render thread). Layers: 0 opaque, 1 late repeat, 2 water/translucent, 3 clouds.
    struct PerfLayer {
        std::uint64_t calls = 0, verts = 0;
        double        ms    = 0; ///< CPU submit time
    };
    std::vector<VisibleSection>           vis_; ///< reused per frame (no per-frame allocation)
    PerfLayer                             perf_[4]{};
    std::uint64_t                         perfFrames_ = 0, perfSections_ = 0, perfLateSkipped_ = 0,
                                          perfClipSkipped_ = 0, perfCloudQuads_ = 0;
    double                                perfMsSum_ = 0, perfMsMax_ = 0;
    std::chrono::steady_clock::time_point lastPerf_{};
    void                                  logPerf(std::chrono::steady_clock::time_point now);
};

} // namespace dh::render
