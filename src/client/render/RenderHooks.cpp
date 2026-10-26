#include "client/render/RenderHooks.h"

#include "client/SettingsPage.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/hud/DebugHud.h"
#include "client/render/LodRenderer.h"
#include "client/render/TestFeed.h"
#include "client/spikes/SpikeGfx.h"
#include "client/world/ClientWorldDriver.h"
#include "dh/config/Options.h"
#include "dh/render/RenderUtil.h"

#include <optional>
#include <string>
#include <tuple>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/minecraft_renderer/objects/FrameRenderObject.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // std::getenv

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

namespace dh::render {

namespace {

std::atomic<bool>            gEnabled{false};
bool                         gInstalled = false;
std::unique_ptr<LodRenderer> gRenderer;
ll::event::ListenerPtr       gKeyListener;
ll::event::ListenerPtr       gExitListener;
bool                         gDebugKeys   = false; // client.debug.enabled
int                          gToggleKey   = 0;     // client.keys.toggleRendering (VK), 0 = none
bool                         gFeedAllowed = false; // DH_TEST_FEED=1

enum class VFogMode { Off, PushDistanceFog, PushFogControl, All };
VFogMode gVFog = VFogMode::All;

char const* vfogName(VFogMode m) {
    switch (m) {
    case VFogMode::Off:
        return "off";
    case VFogMode::PushDistanceFog:
        return "distance-fog";
    case VFogMode::PushFogControl:
        return "fog-control";
    default:
        return "all";
    }
}

void updateHud() {
    auto& r = *gRenderer;
    dh::hud::set("render.enabled", gEnabled ? "on" : "off");
    dh::hud::set("render.material", r.materialName());
    dh::hud::set("render.far", r.farModeName());
    dh::hud::set("render.hook", r.drawHookName());
    dh::hud::set("render.vanillaFog", vfogName(gVFog));
    dh::hud::set("render.feed", testfeed::active() ? "on" : "off");
    if (!gEnabled) dh::hud::set("render.clouds", r.cloudsWanted() ? "on (pipeline off)" : "off");
}

void guardedDraw(ScreenContext& sc, IClientInstance& ci, char const* where) {
    try {
        gRenderer->drawFrame(sc, gRenderer->lastView(), ci);
    } catch (std::exception const& e) {
        static int n = 0;
        if (n++ < 5) DH_LOG_E("render: %s draw threw: %s", where, e.what());
    } catch (...) {
        static int n = 0;
        if (n++ < 5) DH_LOG_E("render: %s draw threw an unknown exception", where);
    }
}

// The IClientInstance for the LevelRenderer-based hooks (they do not receive one).
IClientInstance* gClient = nullptr;


// ---- vanilla clouds (Java DH: vanilla clouds render on top of the LODs, so they are disabled while DH draws) ------
// Hook result (Run 10): there is no MCAPI cloud-draw function in the SDK (LevelRendererPlayer/Camera only expose the
// cloud mesh fields), but the per-frame view data the renderer consumes carries ViewRenderData::mDrawClouds. We clear
// it on every non-shadow view before the vanilla world is drawn (LevelRenderer::renderLevel and
// LevelRendererCamera::$render, whichever sees the view first) and put it back right after, so vanilla comes back the
// frame the pipeline / DH clouds turn off. Hidden only once the DH cloud layer has actually been drawn (hand-off).
/// Puts mDrawClouds back on destruction (also when the wrapped original throws).
struct CloudRestore {
    std::vector<ViewRenderObject*> views;
    ~CloudRestore() {
        for (auto* v : views) v->mViewData.get().mDrawClouds = true;
    }
};

bool shouldHideVanillaClouds() {
    if (!gRenderer || !gEnabled || !bedrock::ClientWorldDriver::instance().pipeline()) return false;
    if (!bedrock::ConfigBridge::instance().config().options().client.graphics.clouds.hideVanillaClouds) return false;
    return gRenderer->cloudsWanted() && gRenderer->cloudsRecentlyDrawn();
}

/// Clears mDrawClouds on `vro` if vanilla clouds should be hidden; true = cleared (restore after the draw).
bool hideVanillaCloudsOn(ViewRenderObject const& vro, char const* where) {
    auto& vd = const_cast<ViewRenderObject&>(vro).mViewData.get();
    if (vd.mIsShadowPass || !vd.mDrawClouds) return false;
    gRenderer->noteVanillaClouds();
    static int lastState = -1;
    const bool hide      = shouldHideVanillaClouds();
    if (lastState != static_cast<int>(hide)) {
        lastState = static_cast<int>(hide);
        DH_LOG_I(
            "clouds: vanilla clouds %s via ViewRenderData::mDrawClouds (%s hook)",
            hide ? "hidden (true -> false)" : "restored (left on)",
            where
        );
    }
    if (!hide) return false;
    vd.mDrawClouds = false;
    return true;
}

// ---- cloud probe (debug key, Run 14) ----------------------------------------------------------------------------------
// One-shot log of the vanilla cloud geometry (box or sheet, thickness, face shades) next to the DH layer's parameters,
// to settle what vanilla Bedrock draws: static RE found the cloud material (no texture, colour = CloudColor * vertex
// colour, alpha fade 0.9..1.9 x DistanceControl.x) but not the CPU mesh builder. mce::MeshData may already be freed
// after the GPU upload; then positions=0 is logged and only the AABB/vertex counts are informative.
std::atomic<bool> gCloudProbe{false};

void logCloudProbe(ViewRenderObject const& vro, glm::vec3 const& camPos) {
    auto const& vd  = vro.mViewData.get();
    auto const& cs  = vro.mCloudState.get();
    auto const  col = cs.mCloudColor.get();
    auto const  cc  = cs.mCloudCenterPos.get();
    auto const  co  = cs.mCloudOrigin.get();
    DH_LOG_I(
        "cloud probe: vanilla drawClouds=%d aboveClouds=%d fancy=%d meshSize=%d gridSize=%d yTranslation=%.2f "
        "center=(%.1f,%.1f,%.1f) origin=(%.1f,%.1f,%.1f) color=(%.2f,%.2f,%.2f,%.2f) camera=(%.1f,%.1f,%.1f)",
        static_cast<int>(vd.mDrawClouds),
        static_cast<int>(vd.mCameraAboveClouds),
        static_cast<int>(cs.mIsFancy),
        static_cast<int>(cs.mMeshSize),
        static_cast<int>(cs.mGridSize),
        cs.mYTranslation,
        cc.x, cc.y, cc.z, co.x, co.y, co.z,
        col.r, col.g, col.b, col.a,
        camPos.x, camPos.y, camPos.z
    );
    auto const meshes = cs.mCloudMeshes.get(); // shared_ptr copy keeps the vector alive while we read it
    DH_LOG_I("cloud probe: vanilla meshes=%zu", meshes ? meshes->size() : static_cast<std::size_t>(0));
    if (meshes) {
        std::size_t idx = 0;
        for (auto const& mesh : *meshes) {
            if (idx >= 4) break;
            auto const& md  = mesh.mMeshData.get();
            auto const& pos = md.mPositions.get();
            auto const& cl  = md.mColors.get();
            auto const& bb  = md.mAABB.get();
            auto const  vc  = mesh.mVertexCount.get();
            glm::vec3   lo{0.0f}, hi{0.0f};
            std::vector<float> ys; // distinct y values (up to 8): 1 = flat sheet, 2 = box top/bottom
            for (std::size_t k = 0; k < pos.size(); ++k) {
                auto const& v = pos[k];
                if (k == 0) lo = hi = v;
                lo = glm::min(lo, v);
                hi = glm::max(hi, v);
                if (ys.size() < 8 && std::find(ys.begin(), ys.end(), v.y) == ys.end()) ys.push_back(v.y);
            }
            std::vector<unsigned> cols; // distinct packed vertex colours (up to 8): baked face shades
            for (auto c : cl)
                if (cols.size() < 8 && std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
            // append with the offset clamped to the buffer, so a truncating snprintf can never push it past the end
            auto append = [](char* buf, std::size_t cap, std::size_t& at, char const* fmt, auto v) {
                if (at + 1 >= cap) return;
                const int n = std::snprintf(buf + at, cap - at, fmt, v);
                if (n > 0) at = std::min(cap - 1, at + static_cast<std::size_t>(n));
            };
            char        ybuf[160] = {0}, cbuf[160] = {0};
            std::size_t yo = 0, co2 = 0;
            for (float y : ys) append(ybuf, sizeof(ybuf), yo, "%.2f ", static_cast<double>(y));
            for (unsigned c : cols) append(cbuf, sizeof(cbuf), co2, "%08X ", c);
            DH_LOG_I(
                "cloud probe: mesh %zu prim=%d vertexCount=%d raw=%zu positions=%zu indices=%zu colors=%zu "
                "pos[min=(%.1f,%.1f,%.1f) max=(%.1f,%.1f,%.1f)] aabb[(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)] ys={%s} cols={%s}",
                idx,
                static_cast<int>(mesh.mPrimitiveMode),
                vc ? static_cast<int>(*vc) : -1,
                mesh.mRawData.get().size(),
                pos.size(),
                md.mIndices.get().size(),
                cl.size(),
                lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                bb.first.x, bb.first.y, bb.first.z, bb.second.x, bb.second.y, bb.second.z,
                ybuf,
                cbuf
            );
            ++idx;
        }
    }
    if (gRenderer) {
        auto const& cr = gRenderer->clouds();
        DH_LOG_I(
            "cloud probe: DH layer wanted=%d drawn=%d quads=%zu slab y %.0f..%.0f (cell %d, alpha %.2f) backFaceCull=%s "
            "(active if camera-relative=%d)",
            static_cast<int>(gRenderer->cloudsWanted()),
            static_cast<int>(cr.stats().drawn),
            cr.stats().quads,
            static_cast<double>(cr.slabBottom()),
            static_cast<double>(cr.slabBottom() + dh::render::CloudRenderer::kThickness),
            dh::render::CloudRenderer::kCellBlocks,
            static_cast<double>(dh::render::CloudRenderer::kAlpha),
            dh::render::CloudRenderer::backFaceCullEnabled() ? "on" : "off",
            static_cast<int>(cr.cullsBackFaces(camPos.y, true))
        );
    }
}

// Draw the LODs right after the vanilla world (same hook point as spike R1); also caches the view every frame.
LL_TYPE_INSTANCE_HOOK(
    LodRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$render,
    void,
    ::BaseActorRenderContext& rc,
    ::ViewRenderObject const& vro,
    ::IClientInstance&        ci
) {
    CloudRestore restore;
    if (gRenderer) {
        try {
            auto const& vd = vro.mViewData.get();
            if (!vd.mIsShadowPass) {
                const float h = static_cast<float>(mCloudHeight);
                if (h > 0.0f && h < 4096.0f) gRenderer->noteCloudHeight(h);
                static float loggedH = -1.0f;
                if (h != loggedH) {
                    loggedH         = h;
                    auto const& cs  = vro.mCloudState.get();
                    auto const  col = cs.mCloudColor.get();
                    DH_LOG_I(
                        "clouds: LevelRendererCamera::mCloudHeight=%d mDrawClouds=%d aboveClouds=%d | "
                        "CloudRenderObject "
                        "fancy=%d meshSize=%d gridSize=%d yTranslation=%.2f color=(%.2f,%.2f,%.2f,%.2f)",
                        static_cast<int>(h),
                        static_cast<int>(vd.mDrawClouds),
                        static_cast<int>(vd.mCameraAboveClouds),
                        static_cast<int>(cs.mIsFancy),
                        static_cast<int>(cs.mMeshSize),
                        static_cast<int>(cs.mGridSize),
                        cs.mYTranslation,
                        col.r,
                        col.g,
                        col.b,
                        col.a
                    );
                }
            }
            if (!vd.mIsShadowPass && gCloudProbe.exchange(false)) {
                try {
                    auto const& p = vd.mCameraPos.get();
                    logCloudProbe(vro, glm::vec3(p.x, p.y, p.z));
                } catch (...) {
                    DH_LOG_E("cloud probe: threw");
                }
            }
            if (hideVanillaCloudsOn(vro, "camera")) restore.views.push_back(&const_cast<ViewRenderObject&>(vro));
        } catch (...) {
            static int n = 0;
            if (n++ < 3) DH_LOG_E("clouds: camera hook threw");
        }
    }
    origin(rc, vro, ci);
    if (!gRenderer) return;
    gClient = &ci;
    gRenderer->noteView(vro);
    if (!gEnabled) {
        gRenderer->observeCamera(vro); // so the test feed can be placed before the first drawn frame
        return;
    }
    if (gRenderer->config().drawHook != DrawHook::CameraAfter) return;
    try {
        gRenderer->renderFrame(rc, vro, ci);
    } catch (std::exception const& e) {
        static int n = 0;
        if (n++ < 5) DH_LOG_E("render: renderFrame threw: %s", e.what());
    } catch (...) {
        static int n = 0;
        if (n++ < 5) DH_LOG_E("render: renderFrame threw an unknown exception");
    }
}

LL_TYPE_INSTANCE_HOOK(
    LodLevelRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::renderLevel,
    void,
    ::ScreenContext&           sc,
    ::FrameRenderObject const& fro
) {
    DrawHook m = gRenderer ? gRenderer->config().drawHook : DrawHook::CameraAfter;
    bool     active =
        gRenderer && gEnabled && gClient && gRenderer->lastView().valid && !gRenderer->lastView().isShadowPass;
    if (active && m == DrawHook::LevelRenderBefore) guardedDraw(sc, *gClient, "level-before");
    CloudRestore restoreViews;
    if (gRenderer) {
        try {
            for (auto& v : const_cast<FrameRenderObject&>(fro).mViews.get())
                if (hideVanillaCloudsOn(v, "level")) restoreViews.views.push_back(&v);
        } catch (...) {
            static int n = 0;
            if (n++ < 3) DH_LOG_E("clouds: level hook threw");
        }
    }
    origin(sc, fro);
    if (active && m == DrawHook::LevelRenderAfter) guardedDraw(sc, *gClient, "level-after");
}

LL_TYPE_INSTANCE_HOOK(
    LodPostLevelHook,
    ll::memory::HookPriority::Normal,
    InGamePlayScreen,
    &InGamePlayScreen::$_postLevelRender,
    void,
    ::ScreenContext& sc,
    ::LevelRenderer& lr
) {
    origin(sc, lr);
    if (!gRenderer || !gEnabled || !gClient || gRenderer->config().drawHook != DrawHook::PostLevelRender) return;
    if (!gRenderer->lastView().valid || gRenderer->lastView().isShadowPass) return;
    guardedDraw(sc, *gClient, "post-level");
}

// far-plane mode "zfar-override": enlarge mZFar and rebuild the projection top after the camera is set up.
LL_TYPE_INSTANCE_HOOK(
    LodSetupCameraHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,
    void,
    ::mce::Camera& cam,
    float          a
) {
    origin(cam, a);
    if (!gEnabled || !gRenderer || gRenderer->config().farMode != FarMode::ZFarOverride) return;
    try {
        float      oldFar = cam.mZFar;
        float      newFar = gRenderer->config().farBlocks;
        glm::mat4  p      = cam.projectionMatrixStack->top()._m;
        auto       info   = spikes::analyzeProjection(p, cam.mZNear, oldFar);
        static int logged = 0;
        if (!info.valid) {
            if (logged++ < 3) DH_LOG_W("render: setupCamera: projection layout not recognised, mZFar override skipped");
            return;
        }
        if (oldFar >= newFar) return; // already enlarged (e.g. by the spike hook)
        cam.mZFar                              = newFar;
        cam.projectionMatrixStack->getTop()._m = spikes::patchFar(p, info, cam.mZNear, oldFar, newFar);
        cam.updateViewMatrixDependencies();
        if (logged++ < 3) DH_LOG_I("render: setupCamera: mZFar %.1f -> %.1f (%s)", oldFar, newFar, info.kind);
    } catch (...) {
        static int n = 0;
        if (n++ < 3) DH_LOG_E("render: setupCamera hook threw");
    }
}

// ---- vanilla fog (client.graphics.fog.vanillaFog) ----------------------------------------------------------------
// Experiment: which LevelRendererCamera fog fields feed the shader uniforms is unknown, so the write set is a mode.
float lodFarBlocks() {
    float rad = static_cast<float>(
        bedrock::ConfigBridge::instance().config().options().client.graphics.quality.lodChunkRenderDistanceRadius
    );
    return renderutil::farClipPlaneDistanceInBlocks(static_cast<std::int32_t>(rad));
}

LL_TYPE_INSTANCE_HOOK(
    VanillaFogHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$setupFog,
    void,
    ::ScreenContext& sc,
    float const      sky
) {
    origin(sc, sky);
    try {
        if (gVFog == VFogMode::Off || !bedrock::ClientWorldDriver::instance().pipeline()) return;
        // Java keeps vanilla fog in fluids / blindness. We cannot tell directly: those fogs are short (< 48 blocks),
        // normal distance fog ends near the render distance, so only push when the fog is a far one.
        float oldEnd = mCurrentDistanceFog->mEnd;
        if (oldEnd < 48.0f) return;
        float      farB   = lodFarBlocks();
        float      end    = farB;
        float      start  = farB * 0.9f;
        bool       dist   = gVFog == VFogMode::PushDistanceFog || gVFog == VFogMode::All;
        bool       ctrl   = gVFog == VFogMode::PushFogControl || gVFog == VFogMode::All;
        static int logged = -1;
        if (logged != static_cast<int>(gVFog)) {
            logged = static_cast<int>(gVFog);
            DH_LOG_I(
                "vfog: mode %s before: cur[%.1f,%.1f] last[%.1f,%.1f] base[%.1f,%.1f] ctrl[%.1f,%.1f] lastEnd %.1f "
                "blend %d type %d -> push [%.1f,%.1f]",
                vfogName(gVFog),
                mCurrentDistanceFog->mStart,
                mCurrentDistanceFog->mEnd,
                mLastTargetDistanceFog->mStart,
                mLastTargetDistanceFog->mEnd,
                mBaseFogStart,
                mBaseFogEnd,
                mFogControl->x,
                mFogControl->y,
                mLastFogEnd,
                (int)mBlendFogThisFrame,
                (int)mCurrentDistanceFog->mType,
                start,
                end
            );
        }
        if (dist) {
            mCurrentDistanceFog->mStart    = start;
            mCurrentDistanceFog->mEnd      = end;
            mLastTargetDistanceFog->mStart = start;
            mLastTargetDistanceFog->mEnd   = end;
            mBaseFogStart                  = start;
            mBaseFogEnd                    = end;
            mLastFogEnd                    = end;
            mBlendFogThisFrame             = false;
        }
        if (ctrl) {
            mFogControl->x = start;
            mFogControl->y = end;
        }
    } catch (...) {
        static int n = 0;
        if (n++ < 3) DH_LOG_E("vfog: hook threw");
    }
}

/// Pipeline on/off (the whole thing: world driver hooks -> DH world -> quad tree, plus the renderer).
void setPipeline(LodRenderer& r, bool on) {
    auto& drv = bedrock::ClientWorldDriver::instance();
    if (drv.pipeline() == on) return;
    if (on && r.config().farMode == FarMode::Off) {
        r.config().farMode = FarMode::LocalProjPush; // LODs live beyond the vanilla far plane
        DH_LOG_I("render: far-plane mode -> local-proj-push (pipeline turned on)");
    }
    drv.setPipeline(on); // also calls setEnabled()
    DH_LOG_I("render: pipeline %s", on ? "on" : "off");
}

/// The toggle key: like Java's quickEnableRendering key it flips the option itself, so the setting persists and
/// the settings page shows the real state.
void togglePipeline(LodRenderer& r) {
    const bool on = !bedrock::ClientWorldDriver::instance().pipeline();
    setPipeline(r, on);
    auto& store = bedrock::ConfigBridge::instance().config();
    if (store.options().client.enableRendering != on) {
        store.set([on](dh::Options& o) { o.client.enableRendering = on; });
        dh::settings::notifyChanged("client.enableRendering");
    }
}

/// The synthetic test feed (Stage-A smoke path, only with DH_TEST_FEED=1): Shift+F5.
void toggleTestFeed(LodRenderer& r) {
    glm::dvec3 pos;
    if (!r.lastCameraPos(pos)) {
        DH_LOG_W("render: test feed: no camera position seen yet (no world frame rendered?)");
        return;
    }
    if (!gEnabled && !testfeed::active()) {
        gEnabled = true;
        DH_LOG_I("render: Stage-A renderer on (enabled by test feed)");
    }
    testfeed::toggle(r, pos);
    r.config().drawAllUploaded = testfeed::active(); // feed sections are in no quad tree
    DH_LOG_I("render: test feed %s", testfeed::active() ? "on" : "off");
}

/// Numpad debug map (client.debug.enabled only). The overlay key and Numpad . (HUD dump) live in DebugHud.
void debugKeys(LodRenderer& r) {
    using dh::input::pressed;
    if (pressed(VK_NUMPAD1)) {
        gVFog = static_cast<VFogMode>((static_cast<int>(gVFog) + 1) % 4);
        DH_LOG_I("vfog: mode -> %s", vfogName(gVFog));
    }
    if (pressed(VK_NUMPAD2)) r.toggleClouds();
    if (pressed(VK_NUMPAD3)) r.cycleLightmapMode();
    if (pressed(VK_NUMPAD4)) r.cycleSkyOrder();
    if (pressed(VK_NUMPAD5)) {
        gCloudProbe = true; // logged by the next non-shadow camera hook (it has the ViewRenderObject)
        DH_LOG_I("cloud probe: requested");
    }
    if (pressed(VK_NUMPAD6)) dh::render::CloudRenderer::toggleBackFaceCull();
    if (pressed(VK_NUMPAD7)) r.toggleLateScope();
    if (pressed(VK_NUMPAD8)) r.toggleNearCutoff();
    if (pressed(VK_NUMPAD9)) r.cycleFarMode();
    if (pressed(VK_DIVIDE)) r.cycleMaterial();
    if (pressed(VK_MULTIPLY)) r.cycleDrawHook();
    if (pressed(VK_SUBTRACT)) {
        auto& drv = bedrock::ClientWorldDriver::instance();
        drv.setIngestion(!drv.ingestion());
    }
    if (pressed(VK_ADD)) {
        r.config().coreFrustum = !r.config().coreFrustum;
        DH_LOG_I("render: core RenderList frustum cull -> %s", r.config().coreFrustum ? "on" : "off");
    }
}

void onKeys(ll::event::AfterUIRenderEvent& e) {
    if (!gRenderer || e.screenView().getScreenName() != "hud_screen") return;
    try {
        auto& r = *gRenderer;
        // keep the "draw everything uploaded" switch tied to the feed's real state (reset() paths clear the feed)
        r.config().drawAllUploaded = testfeed::active();
        if (gToggleKey != 0 && dh::input::pressed(gToggleKey)) togglePipeline(r);
        if (gFeedAllowed && dh::input::shiftDown() && dh::input::pressed(VK_F5)) toggleTestFeed(r);
        if (gDebugKeys) {
            debugKeys(r);
            updateHud();
        }
    } catch (...) {
        static int n = 0;
        if (n++ < 3) DH_LOG_E("render: key handler threw");
    }
}

VFogMode fogModeOf(dh::VanillaFogMode m) {
    switch (m) {
    case dh::VanillaFogMode::Off:
        return VFogMode::Off;
    case dh::VanillaFogMode::Distance:
        return VFogMode::PushDistanceFog;
    case dh::VanillaFogMode::FogControl:
        return VFogMode::PushFogControl;
    default:
        return VFogMode::All;
    }
}

} // namespace

namespace {

/// The options that take effect without rejoining a world. Compared on every store change so an unrelated edit
/// (a slider on another page) does not re-apply the render config and undo what the debug keys toggled at runtime.
struct RuntimeOptions {
    dh::AdvancedOptions advanced;
    dh::DebugOptions    debug;
    bool                backFaceCull;
    dh::VanillaFogMode  vanillaFog;
    bool                enableRendering;
    std::string         toggleKey;

    static RuntimeOptions of(const dh::Options& o) {
        return {
            o.client.advanced,
            o.client.debug,
            o.client.graphics.clouds.backFaceCull,
            o.client.graphics.fog.vanillaFog,
            o.client.enableRendering,
            o.client.keys.toggleRendering
        };
    }
    auto tie() const {
        return std::tie(
            advanced.skyOrder, advanced.lateScope, advanced.lightmapMode, advanced.nearClip, advanced.farPlane,
            advanced.material, advanced.drawHook, advanced.coreFrustum, advanced.drawTransparent,
            debug.enabled, debug.hudKey, debug.logRenderPerf, debug.logLightingDiag,
            backFaceCull, vanillaFog, enableRendering, toggleKey
        );
    }
    bool operator==(const RuntimeOptions& o) const { return tie() == o.tie(); }
};
std::optional<RuntimeOptions> gApplied; // what applyRuntimeOptions last saw

/// Applies the runtime options. Runs at install and on every change of the store (the settings page, the toggle
/// key); a no-op when none of them changed.
void applyRuntimeOptions(const dh::Options& opts) {
    if (!gRenderer) return;
    RuntimeOptions now = RuntimeOptions::of(opts);
    if (gApplied && *gApplied == now) return;
    gApplied = now;
    gRenderer->applyOptions(opts);
    gVFog      = fogModeOf(opts.client.graphics.fog.vanillaFog);
    gDebugKeys = opts.client.debug.enabled;
    gToggleKey = dh::input::resolveKey(opts.client.keys.toggleRendering, "client.keys.toggleRendering");
    setPipeline(*gRenderer, opts.client.enableRendering);
    if (bedrock::ClientWorldDriver::instance().pipeline() && gRenderer->config().farMode == FarMode::Off)
        gRenderer->config().farMode = FarMode::LocalProjPush; // LODs live beyond the vanilla far plane
    // The key poll only exists when something needs it: a bound key, the debug keys or the env test feed.
    if ((gToggleKey != 0 || gDebugKeys || gFeedAllowed) && !gKeyListener)
        gKeyListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::AfterUIRenderEvent>(onKeys);
    if (gDebugKeys) updateHud();
}

bool gStoreListenerAdded = false; // ConfigStore listeners cannot be removed: register once per process

} // namespace

LodRenderer& renderer() { return *gRenderer; }
bool         enabled() { return gEnabled; }
void         setEnabled(bool on) {
    gEnabled = on;
    if (gRenderer) updateHud();
}

void install() {
    if (gInstalled) return;
    gInstalled = true;
    gRenderer  = std::make_unique<LodRenderer>();

    // Options (config.json is loaded by ConfigBridge::init in DistantHorizons::enable before this runs).
    auto&             store = bedrock::ConfigBridge::instance().config();
    const dh::Options opts  = store.options();
    gRenderer->applyOptions(opts);
    gApplied   = RuntimeOptions::of(opts);
    gVFog      = fogModeOf(opts.client.graphics.fog.vanillaFog);
    gDebugKeys = opts.client.debug.enabled;
    gToggleKey = dh::input::resolveKey(opts.client.keys.toggleRendering, "client.keys.toggleRendering");
    {
        char const* feed = std::getenv("DH_TEST_FEED");
        gFeedAllowed     = feed != nullptr && feed[0] == '1';
    }
    // The pipeline itself is started by the world driver (client.enableRendering); from here on every change of
    // the store re-applies the runtime options.
    if (!gStoreListenerAdded) {
        gStoreListenerAdded = true;
        store.onChange([](const dh::Options& o) {
            if (gInstalled) applyRuntimeOptions(o);
        });
    }

    LodRenderHook::hook();
    LodLevelRenderHook::hook();
    LodPostLevelHook::hook();
    LodSetupCameraHook::hook();
    VanillaFogHook::hook();
    auto& bus = ll::event::EventBus::getInstance();
    // The key poll only exists when something needs it: a bound key, the debug keys or the env test feed.
    if (gToggleKey != 0 || gDebugKeys || gFeedAllowed)
        gKeyListener = bus.emplaceListener<ll::event::AfterUIRenderEvent>(onKeys);
    gExitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
        if (gRenderer) {
            gRenderer->sink().requestClear();
            gRenderer->config().drawAllUploaded = false;
        }
        testfeed::reset();
    });
    if (gDebugKeys) updateHud();

    std::string keys = "toggleRendering=" + (gToggleKey != 0 ? opts.client.keys.toggleRendering : std::string("none"));
    if (gDebugKeys) {
        keys += ", debug on (overlay=" + opts.client.debug.hudKey +
                ", numpad 1-9 / . / + - * keys active, see docs/DEBUG.md)";
    } else {
        keys += ", debug off";
    }
    if (gFeedAllowed) keys += ", test feed Shift+F5 (DH_TEST_FEED=1)";
    DH_LOG_I("keys: %s", keys.c_str());
}

void uninstall() {
    if (!gInstalled) return;
    gInstalled = false;
    gEnabled   = false;
    gApplied.reset();
    auto& bus  = ll::event::EventBus::getInstance();
    if (gKeyListener) bus.removeListener(gKeyListener);
    if (gExitListener) bus.removeListener(gExitListener);
    gKeyListener.reset();
    gExitListener.reset();
    LodSetupCameraHook::unhook();
    LodPostLevelHook::unhook();
    LodLevelRenderHook::unhook();
    gClient = nullptr;
    LodRenderHook::unhook();
    testfeed::reset();
    gRenderer.reset(); // meshes are leaked on purpose (MeshUploader dtor): not on the render thread
}

} // namespace dh::render
