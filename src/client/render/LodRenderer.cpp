#include "client/render/LodRenderer.h"

#include "bedrock_common/FileLogger.h"
#include "client/hud/DebugHud.h"
#include "client/render/Frustum5.h"
#include "client/render/MaterialInjector.h"
#include "client/spikes/SpikeGfx.h"
#include "client/world/ClientWorldDriver.h"

#include "bedrock_common/ConfigBridge.h"
#include "dh/config/Options.h"
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

} // namespace

LodRenderer::LodRenderer() { applyMeshSettings(); }

void LodRenderer::applyMeshSettings() {
    MeshSettings s;
    s.prim                    = Primitive::QuadList; // fixed: the verified primitive / winding
    s.params.flipWinding      = false;
    s.params.doubleSided      = false;
    s.params.skyBrightness    = cfg_.skyBrightness;
    s.params.cpuLighting      = cpuLighting_;
    s.params.lightmapSwapAxes = lightSwapApplied_;
    s.yOffsetBlocks           = cfg_.minYBlocks;
    uploader_.setSettings(s);
}

void LodRenderer::applyOptions(dh::Options const& o) {
    auto const& a = o.client.advanced;
    switch (a.skyOrder) {
    case dh::SkyOrderMode::Overdraw: cfg_.skyOrder = SkyOrder::Overdraw; break;
    case dh::SkyOrderMode::Late:     cfg_.skyOrder = SkyOrder::Late; break;
    case dh::SkyOrderMode::Opaque:   cfg_.skyOrder = SkyOrder::Opaque; break;
    }
    cfg_.lateScope = a.lateScope == dh::LateScopeMode::Full ? LateScope::Full : LateScope::BeyondVanilla;
    switch (a.lightmapMode) {
    case dh::LightmapSource::Dynamic:        cfg_.lightmapMode = LightmapMode::Dynamic; break;
    case dh::LightmapSource::Shader:         cfg_.lightmapMode = LightmapMode::Shader; break;
    case dh::LightmapSource::DynamicSwapped: cfg_.lightmapMode = LightmapMode::DynamicSwapped; break;
    case dh::LightmapSource::UnlitTest:      cfg_.lightmapMode = LightmapMode::UnlitTest; break;
    case dh::LightmapSource::LightDebug:     cfg_.lightmapMode = LightmapMode::LightDebug; break;
    }
    cfg_.nearCutoff = a.nearClip;
    switch (a.farPlane) {
    case dh::FarPlaneMode::LocalProjPush: cfg_.farMode = FarMode::LocalProjPush; break;
    case dh::FarPlaneMode::ZFarOverride:  cfg_.farMode = FarMode::ZFarOverride; break;
    case dh::FarPlaneMode::Off:           cfg_.farMode = FarMode::Off; break;
    }
    switch (a.material) {
    case dh::MaterialMode::Dh:           cfg_.material = MaterialChoice::Dh; break;
    case dh::MaterialMode::MeshFallback: cfg_.material = MaterialChoice::MeshFallback; break;
    case dh::MaterialMode::Selection:    cfg_.material = MaterialChoice::Selection; break;
    }
    switch (a.drawHook) {
    case dh::DrawHookMode::CameraAfter:       cfg_.drawHook = DrawHook::CameraAfter; break;
    case dh::DrawHookMode::LevelRenderBefore: cfg_.drawHook = DrawHook::LevelRenderBefore; break;
    case dh::DrawHookMode::LevelRenderAfter:  cfg_.drawHook = DrawHook::LevelRenderAfter; break;
    case dh::DrawHookMode::PostLevelRender:   cfg_.drawHook = DrawHook::PostLevelRender; break;
    }
    cfg_.coreFrustum     = a.coreFrustum;
    cfg_.drawTransparent = a.drawTransparent;
    logPerf_             = o.client.debug.logRenderPerf;
    logLightingDiag_     = o.client.debug.logLightingDiag || o.client.debug.enabled;
    CloudRenderer::setBackFaceCull(o.client.graphics.clouds.backFaceCull);
    applyMeshSettings();
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
    DH_LOG_I("render: lightmap mode -> %s", kLightmapModeNames[static_cast<int>(cfg_.lightmapMode)]);
}

char const* LodRenderer::skyOrderName() const {
    switch (cfg_.skyOrder) {
    case SkyOrder::Late:
        return "late";
    case SkyOrder::Overdraw:
        return "overdraw";
    case SkyOrder::Opaque:
        return "opaque";
    }
    return "?";
}

void LodRenderer::cycleSkyOrder() {
    cfg_.skyOrder = static_cast<SkyOrder>((static_cast<int>(cfg_.skyOrder) + 1) % kSkyOrderCount);
    loggedSkyOrder_ = false; // log which material the next frame really uses
    DH_LOG_I("render: sky order -> %s", skyOrderName());
}

char const* LodRenderer::lateScopeName() const {
    return cfg_.lateScope == LateScope::BeyondVanilla ? "beyond-vanilla" : "full";
}

void LodRenderer::toggleLateScope() {
    cfg_.lateScope = cfg_.lateScope == LateScope::BeyondVanilla ? LateScope::Full : LateScope::BeyondVanilla;
    DH_LOG_I("render: late repeat scope -> %s", lateScopeName());
}

bool LodRenderer::cloudsWanted() const {
    return cloudsToggle_ && bedrock::ConfigBridge::instance().config().options().client.graphics.clouds.renderDhClouds;
}

void LodRenderer::toggleClouds() {
    cloudsToggle_ = !cloudsToggle_;
    DH_LOG_I("render: DH clouds -> %s", cloudsToggle_ ? "on" : "off");
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

    // DH_Terrain lighting (LightmapMode, client.advanced.lightmapMode). Default: the procedural Java lightmap in the FS (no texture).
    // Dynamic*: the live RenderDragon lightmap, dynamic texture BrightnessTexture<subClientId> in
    // MinecraftGameplayGraphicsResources::mDynamicTextures (read-only lookup, never creates the entry; see
    // docs/spikes/lightmap-texture.md), bound at slot 0. Without it the draw falls back to the procedural path.
    // LevelRenderer::mBrightnessTex is not tried any more: its client texture is NULL under RenderDragon (Run 11).
    bool               lightmapOk = false;
    char const*        lightSrc   = "none";
    Texture            lightTex{std::monostate{}};
    const LightmapMode lmode    = cfg_.lightmapMode;
    const bool         wantSwap = (lmode == LightmapMode::DynamicSwapped) != cfg_.lightmapSwapAxes;
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
                        "render: lightmap source = dynamic texture BrightnessTexture%u",
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
    // The nibble swap only applies to a bound texture: the procedural FS path always decodes hi = sky, lo = block.
    const bool swapAxes = wantSwap && lightmapOk;
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
    if (logLightingDiag_) {
        if (const auto tnow = std::chrono::steady_clock::now(); tnow - lastLightDiag_ > std::chrono::seconds(10)) {
            lastLightDiag_ = tnow;
            auto const ls  = uploader_.lightStats();
            DH_LOG_I(
                "render: lighting %s src=%s flag=%.2f skyDarken=%.2f cpu=%d v[%.2f..%.2f zero %.0f%%]",
                kLightmapModeNames[static_cast<int>(lmode)],
                lightSrc,
                static_cast<double>(lightFlag),
                static_cast<double>(daylight),
                (int)cpuLighting_,
                static_cast<double>(ls.vMin),
                static_cast<double>(ls.vMax),
                static_cast<double>(ls.zeroFrac * 100.0f)
            );
        }
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
            stats.liveSections -= std::min(stats.liveSections, clouds_.liveSectionCount());
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

    auto& vis = vis_; // member: no per-frame allocation once grown
    vis.clear();
    std::size_t culledFrustum = 0, culledNear = 0, clipSkipped = 0;
    // Run 15: with the DH shader's near clip a section whose farthest corner is nearer than clipRadius (3D, as the
    // FS measures `length(worldPos)` in camera-relative space) has every fragment discarded: skip all its draws.
    const bool clipSkip = dhShader && camRel && clipRadius > 0.0f;
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
        float maxDist = 0.0f;
        {
            const glm::vec3 lo = sec.bmin + t, hi = sec.bmax + t;
            const glm::vec3 rel = camRel ? glm::vec3(0.0f) : glm::vec3(camPos);
            maxDist             = glm::length(glm::max(glm::abs(lo - rel), glm::abs(hi - rel)));
        }
        if (clipSkip && maxDist < clipRadius) {
            ++clipSkipped;
            return;
        }
        glm::vec3 c  = t + (sec.bmin + sec.bmax) * 0.5f;
        float     d2 = camRel
                                ? c.x * c.x + c.z * c.z
                                : static_cast<float>((c.x - camPos.x) * (c.x - camPos.x) + (c.z - camPos.z) * (c.z - camPos.z));
        vis.push_back({&sec, t, d2, maxDist});
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
    // Also the cloud material (clouds keep no depth write whatever the sky order).
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

    // Opaque layer pass bin (SkyOrder, Run 13): DH_TerrainLate lands in the frame graph's Transparent pass, after the
    // stars / sun / moon inserted by the vanilla sky; needs the DH shader and the clone (else the plain opaque draw).
    mce::MaterialPtr const* lateMaterial = nullptr;
    if (dhShader && cfg_.skyOrder != SkyOrder::Opaque && spikes::materialValid(spikes::dhLateMaterial()))
        lateMaterial = &spikes::dhLateMaterial();
    dh::hud::set("render.skyOrder", lateMaterial ? skyOrderName() : "opaque");
    // LOD translucent layer (Run 14). The Transparent pass sorts back to front by an unstable distance key, so with a
    // late opaque repeat the water/ice can be ordered before the floor of its own section. In Overdraw the floor is
    // already resolved in the Opaque pass and DH_TerrainWater writes depth, so a later repeat fails against it.
    mce::MaterialPtr const* waterMaterial = transMaterial;
    if (lateMaterial && cfg_.skyOrder == SkyOrder::Overdraw && spikes::materialValid(spikes::dhWaterMaterial()))
        waterMaterial = &spikes::dhWaterMaterial();
    if (!loggedSkyOrder_ && !vis.empty()) {
        loggedSkyOrder_ = true;
        DH_LOG_I(
            "render: sky order %s -> opaque layer drawn with %s; translucent layer with %s",
            skyOrderName(),
            !lateMaterial                          ? "DH_Terrain (Opaque pass; DH_TerrainLate unavailable or off)"
            : cfg_.skyOrder == SkyOrder::Overdraw ? "DH_Terrain + DH_TerrainLate redraw"
                                                   : "DH_TerrainLate",
            waterMaterial != transMaterial ? "DH_TerrainWater (blend + depth write)"
            : transMaterial != material    ? "DH_TerrainTransparent (blend, no depth write)"
                                           : "the opaque material"
        );
    }

    std::size_t drawnSections = 0, drawCalls = 0, lateSkipped = 0;
    // Late repeat scope (Run 15, LateScope): clipStart = fadeEnd with no dither band, i.e. only the fragments beyond the
    // vanilla render distance are repainted, and sections entirely inside it are not submitted at all.
    const bool  lateBeyond = cfg_.lateScope == LateScope::BeyondVanilla && dhShader && camRel && fadeEnd > 0.0f;
    // never below the Opaque draw's own clip (overdraw tuning > 1 can put clipRadius beyond fadeEnd)
    const float lateClip   = std::floor(std::min(std::max(fadeEnd, clipRadius), 4095.0f) + 0.5f);
    const float packedLate = lateBeyond ? lateClip + lateClip * 4096.0f : packedClip;
    // one sort per frame (near to far); the translucent layer walks it backwards
    std::sort(vis.begin(), vis.end(), [](VisibleSection const& a, VisibleSection const& b) {
        return a.dist2 < b.dist2;
    });
    // `material` (the parameter) shadows the outer opaque material on purpose: the opaque layer is drawn with
    // whichever pass-bin material the SkyOrder picks; countSections = false for the Overdraw repeat; layer = perf_
    // index; late = this is the Overdraw repeat (LateScope applies).
    auto drawLayer = [&](bool transparent, mce::MaterialPtr const* material, bool countSections, int layer, bool late) {
        const auto        lt0    = std::chrono::steady_clock::now();
        const float       packed = late ? packedLate : packedClip;
        const std::size_t n      = vis.size();
        for (std::size_t k = 0; k < n; ++k) {
            auto const& v     = vis[transparent ? n - 1 - k : k];
            auto const& parts = transparent ? v.sec->transparent : v.sec->opaque;
            if (parts.empty()) continue;
            if (late && lateBeyond && v.maxDist < lateClip) {
                ++lateSkipped;
                continue;
            }
            auto   wref = cam.worldMatrixStack->push(false);
            Matrix m    = Matrix::IDENTITY();
            m.translate(v.t.x, v.t.y, v.t.z);
            if (dhShader) {
                // DH_Terrain reads its per-draw parameters from the projective row (glm m[col][row]; HLSL row 3).
                auto& g = m._m.get();
                g[0][3] = packed;
                g[1][3] = fogStart;
                g[2][3] = fogEnd;
                // [3][3]: 1 = lightmap texture at t0; -daylight (<= 0) = procedural lightmap in the FS (see the VS).
                g[3][3] = lightFlag;
            }
            cam.worldMatrixStack->getTop() = m;
            for (auto const& part : parts) {
                part.mesh->renderMesh(
                    mctx,
                    transparent ? *waterMaterial : *material,
                    drawTex,
                    0,
                    part.verts,
                    OffscreenCaptureDescription{},
                    nullptr
                );
                ++drawCalls;
                ++perf_[layer].calls;
                perf_[layer].verts += part.verts;
            }
            if (!transparent && countSections) ++drawnSections;
        }
        perf_[layer].ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lt0).count();
    };
    std::size_t cloudCalls = 0;
    auto        drawAll    = [&] {
        // submission order: opaque LODs (late), then DH water, then clouds. The Transparent pass re-sorts these back to
        // front by a per-draw distance key (Run 14), so this is not necessarily the order the GPU sees.
        if (!lateMaterial || cfg_.skyOrder == SkyOrder::Overdraw) drawLayer(false, material, true, 0, false);
        // the scope only applies to the Overdraw repeat; in `late` this is the only opaque draw
        if (lateMaterial)
            drawLayer(false, lateMaterial, cfg_.skyOrder != SkyOrder::Overdraw, 1, cfg_.skyOrder == SkyOrder::Overdraw);
        if (cfg_.drawTransparent) drawLayer(true, material, true, 2, false);
        if (cloudsReady && transMaterial != material) {
            const auto ct0 = std::chrono::steady_clock::now();
            cloudCalls     = clouds_.draw(mctx, *transMaterial, camPos, camRel, fogStart, fogEnd, fr);
            perf_[3].ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ct0).count();
            perf_[3].calls += cloudCalls;
            perfCloudQuads_ += clouds_.stats().quadsDrawn;
            // live tiles that are all culled (looking away) still count: vanilla's clouds must stay hidden
            if (cloudCalls || clouds_.stats().tiles) cloudsDrawnNs_ = nowNs();
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
                "QuadList",
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
    dh::hud::set("render.cutoff", static_cast<std::int64_t>(culledNear + clipSkipped));
    dh::hud::set("render.ms", emaMs_);
    dh::hud::set("render.cloudDrawn", static_cast<std::int64_t>(cloudCalls ? clouds_.stats().quadsDrawn : 0));
    dh::hud::set("render.lateScope", lateMaterial && cfg_.skyOrder == SkyOrder::Overdraw ? lateScopeName() : "-");

    ++perfFrames_;
    perfSections_    += vis.size();
    perfLateSkipped_ += lateSkipped;
    perfClipSkipped_ += clipSkipped;
    perfMsSum_       += ms;
    perfMsMax_        = std::max(perfMsMax_, ms);
    if (const auto now = std::chrono::steady_clock::now(); now - lastPerf_ >= std::chrono::seconds(10)) logPerf(now);
}

void LodRenderer::logPerf(std::chrono::steady_clock::time_point now) {
    const bool   first = lastPerf_ == std::chrono::steady_clock::time_point{};
    const double secs  = first ? 0.0 : std::chrono::duration<double>(now - lastPerf_).count();
    if (logPerf_ && secs > 0.0 && perfFrames_ > 0) {
        const double f = static_cast<double>(perfFrames_);
        auto         c = [&](int l) { return static_cast<double>(perf_[l].calls) / f; };
        auto         q = [&](int l) { return static_cast<double>(perf_[l].verts) / 4.0 / f / 1000.0; };
        auto         t = [&](int l) { return perf_[l].ms / f; };
        // per-frame averages over the window (frames = drawFrame calls); layers o = opaque, l = late repeat,
        // w = water/translucent, c = clouds; ms = CPU submit time on the render thread (GPU cost is not measurable)
        DH_LOG_I(
            "render: perf %.0f fps | calls o%.0f l%.0f w%.0f c%.0f | kquads o%.0f l%.0f w%.0f c%.0f | cpu ms %.2f "
            "(max %.2f; o%.2f l%.2f w%.2f c%.2f) | sections %.0f, late skipped %.0f, clip skipped %.0f",
            f / secs,
            c(0),
            c(1),
            c(2),
            c(3),
            q(0),
            q(1),
            q(2),
            static_cast<double>(perfCloudQuads_) / f / 1000.0,
            perfMsSum_ / f,
            perfMsMax_,
            t(0),
            t(1),
            t(2),
            t(3),
            static_cast<double>(perfSections_) / f,
            static_cast<double>(perfLateSkipped_) / f,
            static_cast<double>(perfClipSkipped_) / f
        );
    }
    lastPerf_ = now;
    for (auto& l : perf_) l = PerfLayer{};
    perfFrames_ = perfSections_ = perfLateSkipped_ = perfClipSkipped_ = perfCloudQuads_ = 0;
    perfMsSum_ = perfMsMax_ = 0;
}

} // namespace dh::render
