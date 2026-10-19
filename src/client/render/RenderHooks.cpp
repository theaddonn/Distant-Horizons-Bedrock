#include "client/render/RenderHooks.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/hud/DebugHud.h"
#include "client/render/LodRenderer.h"
#include "client/render/TestFeed.h"
#include "client/spikes/SpikeGfx.h"
#include "client/world/ClientWorldDriver.h"
#include "dh/render/RenderUtil.h"

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
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" // std::getenv

#include <atomic>
#include <memory>
#include <vector>

namespace dh::render {

namespace {

std::atomic<bool>            gEnabled{false};
bool                         gInstalled = false;
std::unique_ptr<LodRenderer> gRenderer;
ll::event::ListenerPtr       gKeyListener;
ll::event::ListenerPtr       gExitListener;

enum class VFogMode { Off, PushDistanceFog, PushFogControl, All };
VFogMode gVFog     = VFogMode::All;
bool     gVFogInit = false;

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
    dh::hud::set("render.origin", r.config().origin == OriginMode::CameraRelative ? "cam-relative" : "absolute");
    dh::hud::set("render.prim", kPrimChoices[r.config().primChoice].name);
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
        gRenderer->observeCamera(vro); // so Shift+F5 can place the test feed before the first drawn frame
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

// ---- vanilla fog (Java DH "disableVanillaFog") ----------------------------------------------------------------
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
        if (!gVFogInit) {
            gVFogInit = true;
            gVFog     = bedrock::ConfigBridge::instance().config().options().client.graphics.fog.disableVanillaFog
                          ? VFogMode::All
                          : VFogMode::Off;
        }
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

void onKeys(ll::event::AfterUIRenderEvent& e) {
    if (e.screenView().getScreenName() != "hud_screen") return;
    try {
        bool  shift = dh::input::shiftDown();
        bool  ctrl  = dh::input::ctrlDown();
        auto& r     = *gRenderer;
        // keep the "draw everything uploaded" switch tied to the feed's real state (reset() paths clear the feed)
        r.config().drawAllUploaded = testfeed::active();
        if (dh::input::pressed(VK_F5)) {
            auto& drv = bedrock::ClientWorldDriver::instance();
            if (shift) {
                // The synthetic test feed (pre-GLUE-4 smoke path) is retired from the key map: it draws every
                // uploaded section and disables the near cut-off, which kept confusing the real-pipeline tests.
                // It stays available with DH_TEST_FEED=1 in the environment for Stage-A debugging.
                static const bool feedAllowed = [] {
                    char const* e = std::getenv("DH_TEST_FEED");
                    return e && e[0] == '1';
                }();
                glm::dvec3 pos;
                if (!feedAllowed) {
                    DH_LOG_I("render: test feed is disabled (set DH_TEST_FEED=1 to enable Shift+F5)");
                } else if (r.lastCameraPos(pos)) {
                    if (!gEnabled && !testfeed::active()) {
                        gEnabled = true;
                        DH_LOG_I("render: Stage-A renderer on (enabled by test feed)");
                    }
                    testfeed::toggle(r, pos);
                    r.config().drawAllUploaded = testfeed::active(); // feed sections are in no quad tree
                    DH_LOG_I("render: test feed %s (Shift+F5)", testfeed::active() ? "on" : "off");
                } else {
                    DH_LOG_W("render: test feed: no camera position seen yet (no world frame rendered?)");
                }
            } else if (ctrl) {
                r.config().coreFrustum = !r.config().coreFrustum;
                DH_LOG_I("render: core RenderList frustum cull -> %s (Ctrl+F5)", r.config().coreFrustum ? "on" : "off");
            } else {
                // F5 = the whole pipeline: world driver (hooks -> DH world -> quad tree) + renderer
                const bool on = !drv.pipeline();
                if (on && r.config().farMode == FarMode::Off) {
                    r.config().farMode = FarMode::LocalProjPush; // LODs live beyond the vanilla far plane
                    DH_LOG_I("render: far-plane mode -> local-proj-push (pipeline on; Shift+F4 cycles)");
                }
                drv.setPipeline(on); // also calls setEnabled()
                DH_LOG_I("render: pipeline %s (F5)", on ? "on" : "off");
            }
        }
        if (dh::input::pressed(VK_F7) && shift) {
            auto& drv = bedrock::ClientWorldDriver::instance();
            drv.setIngestion(!drv.ingestion());
        }
        if (dh::input::pressed(VK_NUMPAD0) || (dh::input::pressed(VK_F8) && ctrl))
            r.cycleDrawHook(); // Numpad 0 (Ctrl+F8 too)
        if (dh::input::pressed(VK_F4)) {
            if (shift) r.cycleFarMode();
            else if (ctrl) r.toggleNearCutoff();
            else r.cyclePrimitive();
        }
        if (dh::input::pressed(VK_F6)) {
            if (shift) r.cycleOrigin();
            else if (ctrl) r.toggleTransparent();
            else r.cycleMaterial();
        }
        if (dh::input::pressed(VK_NUMPAD2)) r.toggleClouds();
        if (dh::input::pressed(VK_NUMPAD1)) {
            gVFogInit = true;
            gVFog     = static_cast<VFogMode>((static_cast<int>(gVFog) + 1) % 4);
            DH_LOG_I("vfog: mode -> %s (Numpad 1)", vfogName(gVFog));
        }
        updateHud();
    } catch (...) {
        static int n = 0;
        if (n++ < 3) DH_LOG_E("render: key handler threw");
    }
}

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
    LodRenderHook::hook();
    LodLevelRenderHook::hook();
    LodPostLevelHook::hook();
    LodSetupCameraHook::hook();
    VanillaFogHook::hook();
    auto& bus     = ll::event::EventBus::getInstance();
    gKeyListener  = bus.emplaceListener<ll::event::AfterUIRenderEvent>(onKeys);
    gExitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
        if (gRenderer) {
            gRenderer->sink().requestClear();
            gRenderer->config().drawAllUploaded = false;
        }
        testfeed::reset();
    });
    updateHud();
    DH_LOG_I(
        "render: Stage-A renderer installed (off; F5 pipeline on/off, Shift+F5 test feed, Ctrl+F5 core frustum, "
        "Shift+F7 ingestion, F4 prim, Shift+F4 far plane, Ctrl+F4 "
        "near cut-off, "
        "F6 material, Shift+F6 origin, Ctrl+F6 transparent, Ctrl+F8 draw hook)"
    );
}

void uninstall() {
    if (!gInstalled) return;
    gInstalled = false;
    gEnabled   = false;
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
