#include "client/render/RenderHooks.h"

#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/hud/DebugHud.h"
#include "client/render/LodRenderer.h"
#include "client/render/TestFeed.h"
#include "client/spikes/SpikeGfx.h"
#include "client/world/ClientWorldDriver.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <memory>

namespace dh::render {

namespace {

std::atomic<bool>            gEnabled{false};
bool                         gInstalled = false;
std::unique_ptr<LodRenderer> gRenderer;
ll::event::ListenerPtr       gKeyListener;
ll::event::ListenerPtr       gExitListener;

void updateHud() {
    auto& r = *gRenderer;
    dh::hud::set("render.enabled", gEnabled ? "on" : "off");
    dh::hud::set("render.material", r.materialName());
    dh::hud::set("render.origin", r.config().origin == OriginMode::CameraRelative ? "cam-relative" : "absolute");
    dh::hud::set("render.prim", kPrimChoices[r.config().primChoice].name);
    dh::hud::set("render.far", r.farModeName());
    dh::hud::set("render.feed", testfeed::active() ? "on" : "off");
}

// Draw the LODs right after the vanilla world (same hook point as spike R1).
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
    origin(rc, vro, ci);
    if (!gRenderer) return;
    if (!gEnabled) {
        gRenderer->observeCamera(vro); // so Shift+F5 can place the test feed before the first drawn frame
        return;
    }
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
        glm::mat4  p      = cam.projectionMatrixStack->top()._m.get();
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
                glm::dvec3 pos;
                if (r.lastCameraPos(pos)) {
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
    LodSetupCameraHook::hook();
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
        "F6 material, Shift+F6 origin, Ctrl+F6 transparent)"
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
    LodRenderHook::unhook();
    testfeed::reset();
    gRenderer.reset(); // meshes are leaked on purpose (MeshUploader dtor): not on the render thread
}

} // namespace dh::render
