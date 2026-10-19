#include "client/render/LodRenderer.h"

#include "bedrock_common/FileLogger.h"
#include "client/hud/DebugHud.h"
#include "client/spikes/SpikeGfx.h"
#include "client/world/ClientWorldDriver.h"

#include "bedrock_common/ConfigBridge.h"
#include "dh/render/RenderUtil.h"

#include "dh/pos/SectionPos.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/MinecraftGameplayGraphicsResources.h"
#include "mc/client/renderer/MinecraftGraphics.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/common/SubClientId.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/utility/NonOwnerPointer.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/DynamicTexture.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshContext.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/minecraft_renderer/resources/mcr/DynamicTextureResourceManager.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/renderer/ShaderColor.h"
#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

namespace dh::render {

namespace {

using Texture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;

/// Visibility planes in the "draw space" the section translate lands in. Five planes only: left, right, bottom, top and
/// "in front of the camera" (clip w > 0). Near/far depth planes are intentionally not used: their convention
/// (reversed-Z, [0,1] vs [-1,1]) is unverified (R3) and LODs have no meaningful far plane.
struct Frustum5 {
    bool      valid = false;
    glm::vec4 p[5]{};

    /// `transposed` = row-vector layout as detected by spikes::analyzeProjection (clip = v * M); see the derivation in
    /// docs/spikes/stage-a-render.md.
    static Frustum5 build(glm::mat4 const& proj, glm::mat4 const& view, bool transposed) {
        Frustum5  f;
        glm::mat4 m   = transposed ? view * proj : proj * view;
        auto      row = [&](int i) { return transposed ? m[i] : glm::row(m, i); };
        glm::vec4 r0 = row(0), r1 = row(1), r3 = row(3);
        f.p[0]  = r3 + r0;
        f.p[1]  = r3 - r0;
        f.p[2]  = r3 + r1;
        f.p[3]  = r3 - r1;
        f.p[4]  = r3;
        f.valid = true;
        for (auto const& pl : f.p)
            if (!std::isfinite(pl.x + pl.y + pl.z + pl.w)) f.valid = false;
        return f;
    }

    bool visible(glm::vec3 const& lo, glm::vec3 const& hi) const {
        for (auto const& pl : p) {
            glm::vec3 v{pl.x >= 0 ? hi.x : lo.x, pl.y >= 0 ? hi.y : lo.y, pl.z >= 0 ? hi.z : lo.z};
            if (pl.x * v.x + pl.y * v.y + pl.z * v.z + pl.w < 0) return false;
        }
        return true;
    }
};

struct Visible {
    MeshUploader::Section const* sec;
    glm::vec3                    t;
    float                        dist2;
};

} // namespace

LodRenderer::LodRenderer() { applyMeshSettings(); }

void LodRenderer::applyMeshSettings() {
    MeshSettings s;
    auto const&  pc           = kPrimChoices[cfg_.primChoice % kPrimChoiceCount];
    s.prim                    = pc.prim;
    s.params.flipWinding      = pc.flip;
    s.params.doubleSided      = pc.doubleSided;
    s.params.skyBrightness    = cfg_.skyBrightness;
    s.params.cpuLighting      = cpuLighting_;
    s.params.lightmapSwapAxes = lightSwapApplied_;
    s.yOffsetBlocks           = cfg_.minYBlocks;
    uploader_.setSettings(s);
}

bool LodRenderer::lastCameraPos(glm::dvec3& out) const {
    if (!haveCam_) return false;
    out = {camX_.load(), camY_.load(), camZ_.load()};
    return true;
}

void LodRenderer::observeCamera(ViewRenderObject const& vro) {
    if (vro.mViewData->mIsShadowPass) return;
    auto const& p = vro.mViewData->mCameraPos.get();
    camX_         = p.x;
    camY_         = p.y;
    camZ_         = p.z;
    haveCam_      = true;
}

char const* LodRenderer::drawHookName() const {
    switch (cfg_.drawHook) {
    case DrawHook::CameraAfter:
        return "camera-after";
    case DrawHook::LevelRenderBefore:
        return "level-before";
    case DrawHook::LevelRenderAfter:
        return "level-after";
    case DrawHook::PostLevelRender:
        return "post-level";
    }
    return "?";
}

void LodRenderer::cycleDrawHook() {
    cfg_.drawHook = static_cast<DrawHook>((static_cast<int>(cfg_.drawHook) + 1) % kDrawHookCount);
    DH_LOG_I("render: draw hook -> %s", drawHookName());
}

char const* LodRenderer::materialName() const {
    switch (cfg_.material) {
    case MaterialChoice::Selection:
        return "selection";
    case MaterialChoice::MeshFallback:
        return "meshFallback";
    case MaterialChoice::Dh:
        return "dh";
    }
    return "?";
}

char const* LodRenderer::farModeName() const {
    switch (cfg_.farMode) {
    case FarMode::Off:
        return "off";
    case FarMode::LocalProjPush:
        return "local-proj-push";
    case FarMode::ZFarOverride:
        return "zfar-override";
    }
    return "?";
}

void LodRenderer::cycleMaterial() {
    cfg_.material = cfg_.material == MaterialChoice::Selection    ? MaterialChoice::MeshFallback
                  : cfg_.material == MaterialChoice::MeshFallback ? MaterialChoice::Dh
                                                                  : MaterialChoice::Selection;
    warnedDh_     = false;
    DH_LOG_I("render: material -> %s", materialName());
}

void LodRenderer::cycleOrigin() {
    cfg_.origin = cfg_.origin == OriginMode::CameraRelative ? OriginMode::Absolute : OriginMode::CameraRelative;
    DH_LOG_I("render: origin mode -> %s", cfg_.origin == OriginMode::CameraRelative ? "camera-relative" : "absolute");
}

void LodRenderer::cyclePrimitive() {
    cfg_.primChoice = (cfg_.primChoice + 1) % kPrimChoiceCount;
    applyMeshSettings();
    DH_LOG_I("render: primitive -> %s (live sections are re-converted)", kPrimChoices[cfg_.primChoice].name);
}

void LodRenderer::cycleFarMode() {
    cfg_.farMode = static_cast<FarMode>((static_cast<int>(cfg_.farMode) + 1) % 3);
    DH_LOG_I("render: far-plane mode -> %s (far=%.0f)", farModeName(), cfg_.farBlocks);
}

void LodRenderer::toggleNearCutoff() {
    cfg_.nearCutoff = !cfg_.nearCutoff;
    loggedCutoff_   = false;
    DH_LOG_I("render: near cut-off -> %s", cfg_.nearCutoff ? "on" : "off");
}

void LodRenderer::cycleLightmapMode() {
    cfg_.lightmapMode = static_cast<LightmapMode>((static_cast<int>(cfg_.lightmapMode) + 1) % kLightmapModeCount);
    DH_LOG_I("render: lightmap mode -> %s (Numpad 3)", kLightmapModeNames[static_cast<int>(cfg_.lightmapMode)]);
}

void LodRenderer::toggleTransparent() {
    cfg_.drawTransparent = !cfg_.drawTransparent;
    DH_LOG_I("render: transparent pass -> %s", cfg_.drawTransparent ? "on" : "off");
}

bool LodRenderer::cloudsWanted() const {
    return cloudsToggle_ && bedrock::ConfigBridge::instance().config().options().client.graphics.clouds.renderDhClouds;
}

void LodRenderer::toggleClouds() {
    cloudsToggle_ = !cloudsToggle_;
    DH_LOG_I("render: DH clouds -> %s (Numpad 2)", cloudsToggle_ ? "on" : "off");
}

namespace {
std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void LodRenderer::noteVanillaClouds() { vanillaCloudsSeenNs_ = nowNs(); }

bool LodRenderer::cloudsRecentlyDrawn() const { return nowNs() - cloudsDrawnNs_.load() < 250'000'000; }

ViewSnapshot LodRenderer::snapshotOf(ViewRenderObject const& vro) {
    ViewSnapshot v;
    auto const&  p   = vro.mViewData->mCameraPos.get();
    v.isShadowPass   = vro.mViewData->mIsShadowPass;
    v.cameraPos      = {p.x, p.y, p.z};
    v.renderDistance = vro.mViewData->mRenderDistance;
    v.skyBrightness  = vro.mViewData->mSkyBrightnessScalar;
    auto const& sky  = vro.mViewData->mSkyColor.get();
    v.skyColor       = {sky.r, sky.g, sky.b};
    v.valid          = true;
    return v;
}

void LodRenderer::noteView(ViewRenderObject const& vro) { lastView_ = snapshotOf(vro); }

void LodRenderer::renderFrame(BaseActorRenderContext& rc, ViewRenderObject const& vro, IClientInstance& ci) {
    drawFrame(rc.mScreenContext, snapshotOf(vro), ci);
}

void LodRenderer::drawFrame(ScreenContext& ctx, ViewSnapshot const& vs, IClientInstance& ci) {
    if (!vs.valid || vs.isShadowPass) return;
    auto t0 = std::chrono::steady_clock::now();

    mce::MeshContext& mctx = ctx;
    auto&             cam  = mctx.camera;

    bedrock::ClientWorldDriver::instance().onFrame(); // fallback tick pump (no-op while the tick event runs)

    // 1. budgeted uploads
    uploader_.pump(ctx, cfg_.maxUploadsPerFrame);

    glm::dvec3 camPos = vs.cameraPos;
    camX_             = camPos.x;
    camY_             = camPos.y;
    camZ_             = camPos.z;
    haveCam_          = true;

    // 2. material
    mce::MaterialPtr const* material = nullptr;
    char const*             matName  = "?";
    auto*                   lr       = ci.getLevelRenderer();
    switch (cfg_.material) {
    case MaterialChoice::Selection:
        if (lr && lr->mLevelRendererPlayer) {
            auto const& m = lr->mLevelRendererPlayer->selectionOpaqueMaterial.get();
            if (spikes::materialValid(m)) {
                material = &m;
                matName  = "selectionOpaqueMaterial";
            }
        }
        break;
    case MaterialChoice::Dh:
        // B3 DH_Terrain: vertex colour, no fog (the FS ignores the fog varying) - the first "LODs beyond the vanilla
        // fog" picture. Falls back to the fogged vanilla material until the injector's splice has been loaded.
        {
            // clone source for the DH mce material if MeshFallback is not loaded yet: the selection material
            mce::MaterialPtr const* alt =
                (lr && lr->mLevelRendererPlayer) ? &lr->mLevelRendererPlayer->selectionOpaqueMaterial.get() : nullptr;
            (void)spikes::dhMaterial(alt);
        }
        if (spikes::materialValid(spikes::dhMaterial())) {
            material = &spikes::dhMaterial();
            matName  = "DH_Terrain";
            if (!loggedDhMaterial_) {
                DH_LOG_I("render: material DH_Terrain resolved (switchable group); drawing LODs with it");
                loggedDhMaterial_ = true;
            }
            break;
        }
        if (!warnedDh_) {
            DH_LOG_W("render: material 'dh' (DH_Terrain) not resolved yet; using meshFallback until it is");
            warnedDh_ = true;
        }
        [[fallthrough]];
    case MaterialChoice::MeshFallback:
        if (spikes::materialValid(spikes::fallbackMaterial())) {
            material = &spikes::fallbackMaterial();
            matName  = "MeshFallbackPosUVNormalColor";
        }
        break;
    }
    if (!material && cfg_.material != MaterialChoice::Selection && lr && lr->mLevelRendererPlayer) {
        // 2026-10-19: MeshFallbackPosUVNormalColor is in materials.index.json but no load list loads it, so the
        // chosen material resolves to nothing. Draw with the selection material (magenta-tinted, unfogged) rather
        // than nothing, and say so once.
        auto const& m = lr->mLevelRendererPlayer->selectionOpaqueMaterial.get();
        if (spikes::materialValid(m)) {
            material = &m;
            matName  = "selectionOpaqueMaterial(auto-fallback)";
            if (!warnedMaterialFallback_) {
                DH_LOG_W("render: material '%s' invalid; drawing with selectionOpaqueMaterial instead", materialName());
                warnedMaterialFallback_ = true;
            }
        }
    }

    // DH_Terrain lighting (LightmapMode, Numpad 3). Default: the procedural Java lightmap in the FS (no texture).
    // Dynamic*: the live RenderDragon lightmap, dynamic texture BrightnessTexture<subClientId> in
    // MinecraftGameplayGraphicsResources::mDynamicTextures (read-only lookup, never creates the entry; see
    // docs/spikes/lightmap-texture.md), bound at slot 0. Without it the draw falls back to the procedural path.
    // (LevelRenderer::mBrightnessTex has a NULL client texture under RenderDragon and is no longer tried.)
    bool               lightmapOk = false;
    char const*        lightSrc   = "none";
    Texture            lightTex{std::monostate{}};
    const LightmapMode lmode    = cfg_.lightmapMode;
    const bool         swapAxes = (lmode == LightmapMode::DynamicSwapped) != cfg_.lightmapSwapAxes;
    if (std::strcmp(matName, "DH_Terrain") == 0
        && (lmode == LightmapMode::Dynamic || lmode == LightmapMode::DynamicSwapped)) {
        if (::MinecraftGraphics* gfx = ci.getMinecraftGraphics().get().get()) {
            auto const& res = gfx->mMinecraftGameplayGraphicsResources.get();
            const auto  key = static_cast<::mce::DynamicTexture>(static_cast<unsigned char>(ci.getClientSubId()));
            std::shared_ptr<::mce::ClientTexture> tex = res.mDynamicTextures->getResource(key);
            // ResourceBlockTemplate::get() is not exported, so only the block's presence can be checked here.
            if (tex && static_cast<bool>(*tex)) {
                if (!loggedLightmapSize_) {
                    DH_LOG_I(
                        "render: lightmap source = dynamic texture BrightnessTexture%u (Numpad 3 cycles the lighting "
                        "path)",
                        static_cast<unsigned>(key)
                    );
                    loggedLightmapSize_ = true;
                }
                lightTex   = *tex;
                lightmapOk = true;
                lightSrc   = "dynamic";
            }
        }
        if (!lightmapOk && !warnedNoLightmap_) {
            DH_LOG_W(
                "render: no dynamic BrightnessTexture for mode %s; drawing DH_Terrain with the procedural lightmap",
                kLightmapModeNames[static_cast<int>(lmode)]
            );
            warnedNoLightmap_ = true;
        }
    }
    // Daylight factor for the procedural lightmap: Dimension::getSkyDarken (Java getSkyDarken: 1 = day, ~0.2 =
    // night, despite the name). ViewRenderData::mSkyBrightnessScalar was 0 in the first run (2026-10-19), so it is
    // only shown on the HUD.
    float daylight = 1.0f;
    if (::LocalPlayer* lp = ci.getLocalPlayer())
        daylight = std::clamp(lp->getDimension().getSkyDarken(1.0f), 0.0f, 1.0f);
    // u_model[0][3][3] for the VS: 1 = lightmap texture at t0, 2 = unlit, -daylight = procedural lightmap.
    const float lightFlag =
        lightmapOk
            ? 1.0f
            : (lmode == LightmapMode::UnlitTest ? 2.0f : (lmode == LightmapMode::LightDebug ? -3.0f : -daylight));
    if (const auto tnow = std::chrono::steady_clock::now(); tnow - lastLightDiag_ > std::chrono::seconds(10)) {
        lastLightDiag_ = tnow;
        auto const ls  = uploader_.lightStats();
        DH_LOG_I(
            "render: lighting diag mode=%s lightmapOk=%d src=%s flag=%.2f skyDarken=%.2f scalar=%.2f cpuLighting=%d "
            "convertedV[min=%.3f max=%.3f zero=%.0f%% valid=%d]",
            kLightmapModeNames[static_cast<int>(lmode)],
            (int)lightmapOk,
            lightSrc,
            static_cast<double>(lightFlag),
            static_cast<double>(daylight),
            static_cast<double>(vs.skyBrightness),
            (int)cpuLighting_,
            static_cast<double>(ls.vMin),
            static_cast<double>(ls.vMax),
            static_cast<double>(ls.zeroFrac * 100.0f),
            (int)ls.valid
        );
    }
    // Lighting mode follows the material: DH_Terrain with a bound lightmap keeps the colours unlit and sends the light
    // in uv0.y; every other case (other materials, no lightmap) applies the CPU light factor. A change re-converts all
    // sections.
    {
        // DH_Terrain always gets unlit colours + packed light (texture lightmap or the procedural one in the FS);
        // only the fallback materials bake a CPU light factor into the vertex colours.
        const bool wantCpu = std::strcmp(matName, "DH_Terrain") != 0;
        dh::hud::set("render.lighting", wantCpu ? "cpu" : (lightmapOk ? "lightmap" : "shader"));
        {
            char day[64];
            std::snprintf(
                day,
                sizeof day,
                "skyDarken=%.2f scalar=%.2f",
                static_cast<double>(daylight),
                static_cast<double>(vs.skyBrightness)
            );
            dh::hud::set("render.daylight", day);
        }
        {
            char hud[96];
            std::snprintf(
                hud,
                sizeof hud,
                "%s: %s%s",
                kLightmapModeNames[static_cast<int>(lmode)],
                lightmapOk ? lightSrc : "none(shader)",
                lightmapOk ? (swapAxes ? " u=sky,v=block" : " u=block,v=sky") : ""
            );
            dh::hud::set("render.lightmap", hud);
        }
        if (wantCpu != cpuLighting_ || lightSwapApplied_ != swapAxes) {
            cpuLighting_      = wantCpu;
            lightSwapApplied_ = swapAxes;
            applyMeshSettings();
        }
    }

    auto stats = uploader_.stats();
    {
        // the cloud section is not terrain: keep it out of the section/byte statistics
        const std::size_t cb = clouds_.liveBytes();
        if (cb) {
            stats.liveSections -= std::min<std::size_t>(stats.liveSections, 1);
            stats.liveBytes    -= std::min(stats.liveBytes, cb);
        }
    }
    dh::hud::set("render.sections", static_cast<std::int64_t>(stats.liveSections));
    dh::hud::set("render.pending", static_cast<std::int64_t>(stats.pending));
    dh::hud::set("render.bytes", static_cast<std::int64_t>(stats.liveBytes));
    dh::hud::set("render.staleRemoves", static_cast<std::int64_t>(stats.staleRemoves));
    if (!material) {
        static int warned = 0;
        if (warned++ < 3) DH_LOG_W("render: material '%s' invalid, nothing drawn", materialName());
        dh::hud::set("render.drawn", 0);
        return;
    }

    // 3. cull
    glm::mat4 proj = cam.projectionMatrixStack->top()._m.get();
    glm::mat4 view = cam.viewMatrixStack->top()._m.get();
    auto      info = spikes::analyzeProjection(proj, cam.mZNear, cam.mZFar);
    Frustum5  fr;
    if (cfg_.frustumCull && info.valid) fr = Frustum5::build(proj, view, info.transposed);

    // 3a. the world pipeline's render list (core frustum cull + near-to-far order), or every uploaded section
    //     (test feed / pipeline off)
    const bool camRel = cfg_.origin == OriginMode::CameraRelative;
    // The vanilla far plane would cull every distant LOD: use the DH far clip (RenderUtil.getFarClipPlane...).
    const float lodRad = static_cast<float>(
        bedrock::ConfigBridge::instance().config().options().client.graphics.quality.lodChunkRenderDistanceRadius
    );
    const float               farBlocks = renderutil::farClipPlaneDistanceInBlocks(static_cast<std::int32_t>(lodRad));
    bedrock::RenderListResult list;
    bedrock::RenderListStatus listStatus = bedrock::RenderListStatus::NoSession;
    {
        dh::RenderListParams params;
        params.isShadowPass = false;
        // The core's Frustum wants glm matrices in the "DhApiMat4f array" layout, i.e. the TRANSPOSE of the
        // column-vector maths matrix (Frustum.h). analyzeProjection tells whether Bedrock's matrices are row-vector
        // (then the glm memory already equals that transpose) or column-vector (then transpose them).
        const bool tr             = info.valid && info.transposed;
        glm::mat4  projFar        = info.valid ? spikes::patchFar(proj, info, cam.mZNear, cam.mZFar, farBlocks) : proj;
        params.mcModelViewMatrix  = tr ? view : glm::transpose(view);
        params.dhProjectionMatrix = tr ? projFar : glm::transpose(projFar);
        // camera-relative mode: the view matrix carries no translation, the core translates by -camera itself;
        // absolute mode: the view matrix already contains it.
        params.exactCameraPosition = camRel ? camPos : glm::dvec3(0.0);
        // In-game (2026-10-19) the Java-layout path above culled every section while Frustum5 (same matrices,
        // convention validated by the drawn quads) was right. Hand the core the column-vector world VP composed
        // exactly like Frustum5::build: row-vector Bedrock matrices -> transpose(view * proj); plus the camera
        // translation the draw path applies per section in camera-relative mode.
        {
            glm::mat4 vp = tr ? glm::transpose(view * projFar) : projFar * view;
            if (camRel) vp = vp * glm::translate(glm::mat4(1.0f), glm::vec3(-camPos));
            params.worldViewProjection    = vp;
            params.hasWorldViewProjection = info.valid;
        }
        listStatus = bedrock::ClientWorldDriver::instance().collectRenderList(params, cfg_.coreFrustum, list);
    }
    // test feed active: its sections belong to no tree, so draw everything the uploader holds
    const bool listMode = listStatus != bedrock::RenderListStatus::NoSession && !cfg_.drawAllUploaded;
    if (listStatus == bedrock::RenderListStatus::Ready && (list.minY != static_cast<int>(cfg_.minYBlocks))) {
        cfg_.minYBlocks = static_cast<float>(list.minY); // DH y 0 == level min height
        applyMeshSettings();
    }
    dh::hud::set("render.listed", static_cast<std::int64_t>(list.positions.size()));

    // camera speed EMA (blocks/s) for the fast-movement overdraw reduction
    {
        const auto now = std::chrono::steady_clock::now();
        if (haveLastCam_) {
            const double dt = std::chrono::duration<double>(now - lastCamTime_).count();
            if (dt > 1e-4 && dt < 1.0) {
                const double dx = camPos.x - lastCamPos_.x, dy = camPos.y - lastCamPos_.y,
                             dz = camPos.z - lastCamPos_.z;
                speedEma_       = speedEma_ * 0.9 + std::sqrt(dx * dx + dy * dy + dz * dz) / dt * 0.1;
            }
        }
        lastCamPos_  = camPos;
        lastCamTime_ = now;
        haveLastCam_ = true;
    }

    // 3b. near cut-off: skip sections entirely inside the area vanilla renders. The radius is the vanilla render
    //     distance scaled by RenderUtil's overdraw prevention (auto table, fast-movement reduction); the
    //     FOV-corrected near PLANE (nearClipPlaneInBlocks) is a projection value and is not used here.
    //     DH_Terrain discards the fragments below the radius instead (Java style; the inner section's top faces then
    //     hide the border overdraw walls), so no section is dropped; the radius travels in u_model's projective row.
    const bool dhShader   = std::strcmp(matName, "DH_Terrain") == 0;
    float      cutoff2    = -1.0f;
    float      clipRadius = 0.0f;
    float      fadeEnd    = 0.0f; // vanilla render distance in blocks: the dither fade band is [clipRadius, fadeEnd]
    if (cfg_.nearCutoff && listMode) {
        float rd = cfg_.vanillaRenderDistanceBlocks;
        if (rd <= 0.0f) {
            rd = vs.renderDistance;
            if (rd < 64.0f) rd *= 16.0f;
        }
        const auto&                cull = bedrock::ConfigBridge::instance().config().options().client.graphics.culling;
        renderutil::NearClipInputs nci;
        nci.overdrawPreventionSetting      = cull.overdrawPrevention;
        nci.reduceOverdrawWithFastMovement = cull.reduceOverdrawWithFastMovement;
        nci.vanillaChunkRenderDistance     = std::max(1, static_cast<std::int32_t>(std::ceil(rd / 16.0f)));
        nci.avgCameraSpeedBlocksPerSec     = speedEma_;
        const float overdraw               = renderutil::overdrawPrevention(nci);
        float r = std::max(1.0f, static_cast<float>(nci.vanillaChunkRenderDistance) * 16.0f * overdraw) * cfg_.overdraw;
        if (dhShader) {
            clipRadius = r;
            fadeEnd    = static_cast<float>(nci.vanillaChunkRenderDistance) * 16.0f;
        } else cutoff2 = r * r;
        if (!loggedCutoff_) {
            DH_LOG_I(
                "render: near cut-off radius %.1f blocks (mRenderDistance=%.2f, overdraw %.2f)",
                r,
                vs.renderDistance,
                overdraw
            );
            loggedCutoff_ = true;
        }
    }

    const float fogEnd   = farBlocks;
    const float fogStart = cfg_.fogStartFraction * farBlocks;

    // 3c. clouds: the section is (re)built off-thread; drawn after the LOD layers below. Only while the vanilla game
    //     would draw clouds itself (cloud setting, dimension) and the blended DH material resolved.
    bool cloudsReady = false;
    {
        const bool vanillaWanted = nowNs() - vanillaCloudsSeenNs_.load() < 1'000'000'000;
        const bool blendOk =
            std::strcmp(matName, "DH_Terrain") == 0 && spikes::materialValid(spikes::dhTransparentMaterial());
        CloudRenderer::Inputs ci_;
        ci_.enable    = cloudsWanted() && vanillaWanted && blendOk;
        ci_.camera    = camPos;
        ci_.farBlocks = farBlocks;
        ci_.cloudY    = cloudHeight_;
        ci_.minY      = cfg_.minYBlocks;
        {
            auto const& sc3 = vs.skyColor;
            const float lum = 0.3f * sc3.r + 0.59f * sc3.g + 0.11f * sc3.b;
            const float br  = std::clamp(lum / 0.5f, 0.15f, 1.0f);
            const float mx  = std::max(std::max(sc3.r, sc3.g), std::max(sc3.b, 1e-3f));
            ci_.tint        = br * (glm::vec3(0.8f) + 0.2f * glm::vec3(sc3.r, sc3.g, sc3.b) / mx);
            if (!loggedCloudTint_ && ci_.enable) {
                loggedCloudTint_ = true;
                DH_LOG_I(
                    "clouds: first update: cloudY=%.0f minY=%.0f far=%.0f sky=(%.2f,%.2f,%.2f) -> "
                    "tint=(%.2f,%.2f,%.2f)",
                    ci_.cloudY,
                    ci_.minY,
                    ci_.farBlocks,
                    sc3.r,
                    sc3.g,
                    sc3.b,
                    ci_.tint.r,
                    ci_.tint.g,
                    ci_.tint.b
                );
            }
        }
        cloudsReady = clouds_.update(ci_) && listStatus != bedrock::RenderListStatus::Unavailable;
        dh::hud::set(
            "render.clouds",
            ci_.enable ? (cloudsReady ? "on" : "building") : (cloudsWanted() ? "waiting" : "off")
        );
        dh::hud::set("render.cloudQuads", static_cast<std::int64_t>(clouds_.stats().quads));
    }
    if (dhShader) {
        dh::hud::set("render.clip", static_cast<double>(clipRadius));
        char fogTxt[48];
        std::snprintf(fogTxt, sizeof fogTxt, "%.0f..%.0f", fogStart, fogEnd);
        dh::hud::set("render.fog", std::string(fogTxt));
    } else {
        dh::hud::set("render.clip", 0.0);
        dh::hud::set("render.fog", std::string("off"));
    }

    std::vector<Visible> vis;
    vis.reserve(listMode ? list.positions.size() : uploader_.sections().size());
    std::size_t culledFrustum = 0, culledNear = 0;
    auto        consider = [&](std::uint64_t pos, MeshUploader::Section const& sec) {
        if (sec.opaque.empty() && sec.transparent.empty()) return;
        if (CloudRenderer::isCloudSection(pos)) return; // drawn by CloudRenderer
        glm::vec3 t = camRel ? glm::vec3(
                                   static_cast<float>(sec.minX - camPos.x),
                                   static_cast<float>(-camPos.y),
                                   static_cast<float>(sec.minZ - camPos.z)
                               )
                                    : glm::vec3(static_cast<float>(sec.minX), 0.0f, static_cast<float>(sec.minZ));
        if (cutoff2 > 0.0f) {
            double w  = sec::blockWidth(pos);
            double x0 = sec.minX - camPos.x, x1 = x0 + w, z0 = sec.minZ - camPos.z, z1 = z0 + w;
            double fx = std::max(std::fabs(x0), std::fabs(x1)), fz = std::max(std::fabs(z0), std::fabs(z1));
            if (fx * fx + fz * fz <= cutoff2) {
                ++culledNear;
                return;
            }
        }
        if (fr.valid && !fr.visible(sec.bmin + t, sec.bmax + t)) {
            ++culledFrustum;
            return;
        }
        glm::vec3 c  = t + (sec.bmin + sec.bmax) * 0.5f;
        float     d2 = camRel
                                ? c.x * c.x + c.z * c.z
                                : static_cast<float>((c.x - camPos.x) * (c.x - camPos.x) + (c.z - camPos.z) * (c.z - camPos.z));
        vis.push_back({&sec, t, d2});
    };
    if (listStatus == bedrock::RenderListStatus::Ready) {
        auto const& all = uploader_.sections();
        for (std::uint64_t pos : list.positions) {
            auto it = all.find(pos);
            if (it != all.end()) consider(pos, it->second);
        }
    } else if (listStatus == bedrock::RenderListStatus::NoSession) {
        for (auto const& [pos, sec] : uploader_.sections()) consider(pos, sec);
    }
    // Unavailable: a session exists but nothing is renderable right now (loading / shutting down): draw nothing.

    // 4. draw
    static Texture const none{std::monostate{}};
    // DH_Terrain: slot 0 (s_MatTexture) = the vanilla lightmap (dynamic BrightnessTexture) in the Dynamic* modes,
    // refreshed by the engine every frame for day/night and gamma. Copy (shared_ptr members), valid for this frame.
    Texture const& drawTex = (dhShader && lightmapOk) ? lightTex : none;
    auto&          sc      = mctx.currentShaderColor;
    mce::Color     saved   = sc.color.get();
    sc.color               = mce::Color{1.f, 1.f, 1.f, 1.f};
    sc.dirty               = true;

    // Transparent layer: blended clone of DH_Terrain (depth test, no depth write); falls back to the opaque material.
    mce::MaterialPtr const* transMaterial = material;
    if (dhShader && spikes::materialValid(spikes::dhTransparentMaterial()))
        transMaterial = &spikes::dhTransparentMaterial();
    if (!loggedTransMaterial_ && cfg_.drawTransparent && !vis.empty()) {
        DH_LOG_I(
            "render: transparent layer material: %s",
            transMaterial != material ? "DH_TerrainTransparent (blended)"
                                      : "same as opaque (DH_TerrainTransparent not resolved)"
        );
        loggedTransMaterial_ = transMaterial != material || !dhShader;
    }
    // [3][0] packs clipStart + fadeEnd * 4096 (both integer blocks <= 4095); see dh_terrain.vert.hlsl.
    const float packedClip = std::floor(std::min(clipRadius, 4095.0f) + 0.5f)
                           + std::floor(std::min(std::max(fadeEnd, clipRadius), 4095.0f) + 0.5f) * 4096.0f;

    std::size_t drawnSections = 0, drawCalls = 0;
    auto        drawLayer = [&](bool transparent) {
        if (transparent)
            std::sort(vis.begin(), vis.end(), [](Visible const& a, Visible const& b) { return a.dist2 > b.dist2; });
        else std::sort(vis.begin(), vis.end(), [](Visible const& a, Visible const& b) { return a.dist2 < b.dist2; });
        for (auto const& v : vis) {
            auto const& parts = transparent ? v.sec->transparent : v.sec->opaque;
            if (parts.empty()) continue;
            auto   wref = cam.worldMatrixStack->push(false);
            Matrix m    = Matrix::IDENTITY();
            m.translate(v.t.x, v.t.y, v.t.z);
            if (dhShader) {
                // DH_Terrain reads its per-draw parameters from the projective row (glm m[col][row]; HLSL row 3).
                auto& g = m._m.get();
                g[0][3] = packedClip;
                g[1][3] = fogStart;
                g[2][3] = fogEnd;
                // [3][3]: 1 = lightmap texture at t0; -daylight (<= 0) = procedural lightmap in the FS (see the VS).
                g[3][3] = lightFlag;
            }
            cam.worldMatrixStack->getTop() = m;
            for (auto const& part : parts) {
                part.mesh->renderMesh(
                    mctx,
                    transparent ? *transMaterial : *material,
                    drawTex,
                    0,
                    part.verts,
                    OffscreenCaptureDescription{},
                    nullptr
                );
                ++drawCalls;
            }
            if (!transparent) ++drawnSections;
        }
    };
    std::size_t cloudCalls = 0;
    auto        drawAll    = [&] {
        drawLayer(false);
        if (cfg_.drawTransparent) drawLayer(true);
        if (cloudsReady && transMaterial != material) {
            cloudCalls = clouds_.draw(mctx, *transMaterial, camPos, camRel, fogStart, fogEnd);
            if (cloudCalls) cloudsDrawnNs_ = nowNs();
        }
    };

    if (!vis.empty() || cloudsReady) {
        if (cfg_.farMode == FarMode::LocalProjPush) {
            if (info.valid) {
                auto pref = cam.projectionMatrixStack->push(false);
                cam.projectionMatrixStack->getTop()._m =
                    spikes::patchFar(proj, info, cam.mZNear, cam.mZFar, cfg_.farBlocks);
                drawAll();
            } else {
                static int warned = 0;
                if (warned++ < 3) DH_LOG_W("render: projection layout not recognised; drawing with vanilla far plane");
                drawAll();
            }
        } else {
            drawAll(); // Off, or ZFarOverride (the setupCamera hook already enlarged the projection)
        }
        if (!loggedFirstDraw_) {
            loggedFirstDraw_ = true;
            // placement probe (2026-10-19, "LODs look too high"): world-space bounds of the first visible sections
            for (std::size_t i = 0; i < std::min<std::size_t>(vis.size(), 4); ++i) {
                auto const& s = *vis[i].sec;
                DH_LOG_I(
                    "render: probe section minX=%d minZ=%d local bmin=(%.1f,%.1f,%.1f) bmax=(%.1f,%.1f,%.1f) -> world "
                    "y "
                    "%.1f..%.1f; camera=(%.1f,%.1f,%.1f) minYBlocks=%.0f",
                    s.minX,
                    s.minZ,
                    s.bmin.x,
                    s.bmin.y,
                    s.bmin.z,
                    s.bmax.x,
                    s.bmax.y,
                    s.bmax.z,
                    s.bmin.y,
                    s.bmax.y,
                    camPos.x,
                    camPos.y,
                    camPos.z,
                    cfg_.minYBlocks
                );
            }
            DH_LOG_I(
                "render: first draw: %zu/%zu sections visible, %zu draw calls, material=%s origin=%s prim=%s far=%s "
                "proj=%s",
                vis.size(),
                stats.liveSections,
                drawCalls,
                matName,
                camRel ? "camera-relative" : "absolute",
                kPrimChoices[cfg_.primChoice].name,
                farModeName(),
                info.valid ? info.kind : "unrecognised"
            );
        }
    }

    sc.color = saved;
    sc.dirty = true;

    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    emaMs_    = emaMs_ == 0 ? ms : emaMs_ * 0.9 + ms * 0.1;
    dh::hud::set("render.drawn", static_cast<std::int64_t>(drawnSections));
    dh::hud::set("render.culled", static_cast<std::int64_t>(culledFrustum));
    dh::hud::set("render.cutoff", static_cast<std::int64_t>(culledNear));
    dh::hud::set("render.ms", emaMs_);
}

} // namespace dh::render
