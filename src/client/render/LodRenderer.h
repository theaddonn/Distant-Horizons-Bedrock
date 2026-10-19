#pragma once

// Stage-A LOD renderer: owns the MeshUploader (the IVertexSink the core uploads into), the draw settings and the
// per-frame draw loop (budgeted upload pump, frustum + near cut-off culling, opaque then transparent draws with a
// per-section world-matrix translate). Render thread only, except sink() which any thread may use.
// All the unknowns R0-R3 are toggles in Config (see docs/spikes/stage-a-render.md).

#include "client/render/CloudRenderer.h"
#include "client/render/MeshUploader.h"

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

class BaseActorRenderContext;
class IClientInstance;
class ScreenContext;
struct ViewRenderObject;

namespace dh::render {

enum class MaterialChoice : int { Selection = 0, MeshFallback = 1, Dh = 2 };
enum class OriginMode : int { CameraRelative = 0, Absolute = 1 };
enum class FarMode : int { Off = 0, LocalProjPush = 1, ZFarOverride = 2 };

/// Primitive / winding choices cycled by F4.
struct PrimChoice {
    Primitive   prim;
    bool        flip;
    bool        doubleSided;
    char const* name;
};
inline constexpr PrimChoice kPrimChoices[] = {
    {Primitive::QuadList,     false, false, "QuadList"                 },
    {Primitive::TriangleList, false, false, "TriangleList"             },
    {Primitive::TriangleList, true,  false, "TriangleList-flipped"     },
    {Primitive::TriangleList, false, true,  "TriangleList-double-sided"},
    {Primitive::QuadList,     false, true,  "QuadList-double-sided"    },
};
inline constexpr int kPrimChoiceCount = static_cast<int>(sizeof(kPrimChoices) / sizeof(kPrimChoices[0]));

/// Where the LOD draw is hooked (Ctrl+F8 cycles).
enum class DrawHook : int { CameraAfter = 0, LevelRenderBefore = 1, LevelRenderAfter = 2, PostLevelRender = 3 };
inline constexpr int kDrawHookCount = 4;

/// The per-frame view data the draw needs, copied from ViewRenderObject::mViewData.
struct ViewSnapshot {
    bool       isShadowPass = false;
    glm::dvec3 cameraPos{0.0};
    float      renderDistance = 0.0f;
    glm::vec3  skyColor{0.5f, 0.7f, 1.0f}; ///< ViewRenderData::mSkyColor (cloud tint)
    bool       valid = false;
};

struct LodRendererConfig {
    DrawHook drawHook = DrawHook::CameraAfter;
    /// selectionOpaqueMaterial drops the green channel (magenta terrain, 2026-10-19); the fogged fallback is the
    /// Stage-A default, F6 cycles back for comparison.
    MaterialChoice material   = MaterialChoice::Dh; // -> MeshFallback (fogged) -> selection, whichever resolves
    OriginMode     origin     = OriginMode::CameraRelative;
    int            primChoice = 0;                      ///< index into kPrimChoices
    FarMode        farMode    = FarMode::LocalProjPush; // R3 (2026-10-19): zfar-override makes the clouds vanish
    float          farBlocks  = 20000.0f;               ///< far plane used by LocalProjPush / ZFarOverride

    std::size_t maxUploadsPerFrame = 8;
    bool        frustumCull        = true;
    bool        drawTransparent    = true;
    bool        coreFrustum        = true; ///< cull in the core RenderList (Ctrl+F5 toggles; off: only Frustum5)
    /// Draw every uploaded section instead of the world pipeline's render list (set while the Shift+F5 test feed
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

    float skyBrightness    = 1.0f;   ///< CPU light stand-in input (0 night .. 1 day); fallback materials only
    bool  lightmapSwapAxes = false;  ///< DH_Terrain: swap the lightmap lookup axes (default u = block, v = sky)
    float minYBlocks       = -64.0f; ///< DH y 0 maps to this Bedrock y
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

    /// Cycle helpers used by the key handler (log the new value).
    void cycleMaterial();
    void cycleOrigin();
    void cyclePrimitive();
    void cycleFarMode();
    void toggleNearCutoff();
    void toggleTransparent();
    void toggleLightmapAxes();
    /// Numpad 2: DH clouds on/off (the vanilla clouds come back while off).
    void toggleClouds();

    // ---- clouds (CloudRenderer) ----
    /// Effective switch: option client.graphics.clouds.renderDhClouds && the Numpad 2 toggle.
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

    bool                      cloudsToggle_ = true;
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
    bool                                  warnedNoLightmap_       = false;
    bool                                  loggedLightmapSize_     = false;
    bool                                  cpuLighting_      = true; ///< ConvertParams::cpuLighting currently applied
    bool                                  lightSwapApplied_ = false;
};

} // namespace dh::render
