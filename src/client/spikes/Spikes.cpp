#include "client/spikes/Spikes.h"

#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/hud/DebugHud.h"
#include "client/spikes/SpikeGfx.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/minecraft_renderer/renderer/MeshContext.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/renderer/ViewportInfo.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

namespace dh::spikes {

namespace {

std::atomic<bool> gDumpR0{false};
std::atomic<bool> gR1{false};
std::atomic<bool> gR1b{false};
std::atomic<int>  gR3{0}; // 0 off, 1 local projection push, 2 mZFar override via setupCamera

struct Anchor {
    bool       set    = false;
    bool       axisZ  = true;
    glm::dvec3 pos{};  // base centre of the 8-blocks-ahead quad (R1)
    glm::dvec3 dir{};  // unit horizontal direction the player was facing
    double     groundY = 0;
};
Anchor gAnchor;

ll::event::ListenerPtr gKeyListener;
ll::event::ListenerPtr gExitListener;
bool                   gInstalled = false;

char const* r3Name(int m) { return m == 0 ? "off" : m == 1 ? "local-proj-push" : "mZFar-override"; }

void updateHud() {
    auto& s = settings();
    dh::hud::set("spike.R1", gR1 ? "on" : "off");
    dh::hud::set("spike.R1b", gR1b ? "on" : "off");
    dh::hud::set("spike.R3", r3Name(gR3));
    dh::hud::set("spike.origin", s.origin == OriginMode::CameraRelative ? "cam-relative" : "absolute");
    dh::hud::set("spike.prim", s.prim == PrimMode::QuadList ? "QuadList" : "TriangleList");
}

void computeAnchor(mce::Camera& cam, glm::dvec3 camPos) {
    glm::dvec3 f{cam.mForward->x, 0.0, cam.mForward->z};
    double     len = std::sqrt(f.x * f.x + f.z * f.z);
    if (len < 1e-4) f = {0, 0, 1};
    else f /= len;
    gAnchor.axisZ = std::fabs(f.z) >= std::fabs(f.x);
    // snap to the dominant axis so the quad faces the player squarely
    gAnchor.dir     = gAnchor.axisZ ? glm::dvec3{0, 0, f.z >= 0 ? 1 : -1} : glm::dvec3{f.x >= 0 ? 1 : -1, 0, 0};
    gAnchor.groundY = std::floor(camPos.y - 1.62);
    gAnchor.pos     = {std::floor(camPos.x) + 0.5, gAnchor.groundY, std::floor(camPos.z) + 0.5};
    gAnchor.set     = true;
    DH_LOG_I(
        "spike anchor: cam=(%.2f,%.2f,%.2f) groundY=%.1f dir=(%.0f,%.0f) plane=%s", camPos.x, camPos.y, camPos.z, gAnchor.groundY,
        gAnchor.dir.x, gAnchor.dir.z, gAnchor.axisZ ? "XY(faces Z)" : "ZY(faces X)"
    );
}

glm::dvec3 at(double ahead, double side, double baseYOffset) {
    // `ahead` blocks along the facing direction, `side` along the in-plane horizontal axis.
    glm::dvec3 p = gAnchor.pos + gAnchor.dir * ahead;
    if (gAnchor.axisZ) p.x += side;
    else p.z += side;
    p.y = gAnchor.groundY + baseYOffset;
    return p;
}

mce::MaterialPtr const& opaqueMaterial(LevelRenderer* lr) {
    if (lr && lr->mLevelRendererPlayer) {
        auto& m = lr->mLevelRendererPlayer->selectionOpaqueMaterial.get();
        if (materialValid(m)) return m;
    }
    return fallbackMaterial();
}

char const* matName(LevelRenderer* lr, mce::MaterialPtr const& m) {
    return &m == &fallbackMaterial() ? "MeshFallbackPosUVNormalColor(fallback)" : (lr ? "selectionOpaqueMaterial" : "?");
}

// Draws every enabled spike quad for the hook `hookName`. `r1Colour` differs per hook (R2).
void drawSpikes(ScreenContext& ctx, LevelRenderer* lr, glm::dvec3 camPos, char const* hookName, bool primaryHook, float r, float g, float b) {
    mce::MeshContext& mctx = ctx;
    auto&             cam  = mctx.camera;
    bool              any  = gR1 || gR1b || gR3 > 0;
    if (!any) return;
    if (!gAnchor.set) computeAnchor(cam, camPos);

    auto const& opaque = opaqueMaterial(lr);
    if (gR1) {
        QuadSpec q;
        q.axisZ = gAnchor.axisZ;
        q.width = q.height = 10;
        q.r = r; q.g = g; q.b = b;
        // primary hook: dead ahead; R2 hooks are shifted sideways so all three quads are visible together.
        double side = primaryHook ? 0.0 : (std::string(hookName) == "LevelRenderer::renderLevel" ? 12.0 : 24.0);
        char   tag[96];
        snprintf(tag, sizeof(tag), "R1/R2 %s mat=%s", hookName, matName(lr, opaque));
        drawQuad(ctx, q, opaque, at(8, side, -5), camPos, tag);
    }
    if (!primaryHook) return;
    if (gR1b) {
        QuadSpec q;
        q.axisZ = gAnchor.axisZ;
        q.width = q.height = 40;
        q.r = 1.0f; q.g = 0.5f; q.b = 0.0f;
        drawQuad(ctx, q, fallbackMaterial(), at(160, 0, -5), camPos, "R1b fogged MeshFallbackPosUVNormalColor @160");
    }
    if (gR3 > 0) {
        QuadSpec q;
        q.axisZ = gAnchor.axisZ;
        q.width = q.height = 400;
        q.r = 0.0f; q.g = 1.0f; q.b = 1.0f;
        char tag[96];
        snprintf(tag, sizeof(tag), "R3 mode=%s quad@1000", r3Name(gR3));
        drawQuad(ctx, q, opaque, at(1000, 0, -100), camPos, tag, gR3 == 1 ? 20000.0f : 0.0f);
    }
}

void dumpInventory(ScreenContext& ctx, ViewRenderObject const& vro, LevelRendererCamera& self, IClientInstance& ci) {
    mce::MeshContext& mctx = ctx;
    auto&             cam  = mctx.camera;
    DH_LOG_I("R0 ===== inventory begin =====");
    DH_LOG_I("R0 vro.mIsShadowPass=%d cameraType=%d", vro.mViewData->mIsShadowPass ? 1 : 0, static_cast<int>(self.getCameraType()));
    DH_LOG_I(
        "R0 vro.mCameraPos=(%.3f,%.3f,%.3f) renderDistance=%.2f", vro.mViewData->mCameraPos->x, vro.mViewData->mCameraPos->y,
        vro.mViewData->mCameraPos->z, vro.mViewData->mRenderDistance
    );
    DH_LOG_I(
        "R0 gfx modules: d3d12=%d d3d11=%d d3d9=%d vulkan-1=%d dxgi=%d", GetModuleHandleW(L"d3d12.dll") ? 1 : 0,
        GetModuleHandleW(L"d3d11.dll") ? 1 : 0, GetModuleHandleW(L"d3d9.dll") ? 1 : 0, GetModuleHandleW(L"vulkan-1.dll") ? 1 : 0,
        GetModuleHandleW(L"dxgi.dll") ? 1 : 0
    );
    auto const& vp = ctx.viewport;
    DH_LOG_I(
        "R0 viewport: size=(%.0f,%.0f) offset=(%.0f,%.0f) minDepth=%.6f maxDepth=%.6f", vp.size->x, vp.size->y, vp.offset->x,
        vp.offset->y, vp.minDepth, vp.maxDepth
    );
    logProjection("R0", cam);
    DH_LOG_I(
        "R0 ci.getCamera()==ctx.camera: %d  forward=(%.3f,%.3f,%.3f)", &ci.getCamera() == &cam ? 1 : 0, cam.mForward->x,
        cam.mForward->y, cam.mForward->z
    );
    logProbe(ctx);

    auto&       group = mce::RenderMaterialGroup::common();
    auto const& mats  = group.mMaterials.get();
    bool        hasRC = false, hasFB = false;
    std::string line;
    // Is the name known to the group at all, and if so is the material object loaded (mPtr)? Distinguishes
    // "not in any load list" from "listed but not loaded" for the names Stage A/B need.
    for (char const* probe : {"MeshFallbackPosUVNormalColor", "DH_Terrain", "RenderChunk", "SelectionOpaque"}) {
        try {
            auto info = group.getMaterialInfo(HashedString{probe});
            auto sw   = mce::RenderMaterialGroup::switchable().getMaterialInfo(HashedString{probe});
            DH_LOG_I(
                "R0 getMaterialInfo(%s): common info=%d loaded=%d | switchable info=%d loaded=%d",
                probe,
                info ? 1 : 0,
                (info && info->mPtr.get()) ? 1 : 0,
                sw ? 1 : 0,
                (sw && sw->mPtr.get()) ? 1 : 0
            );
        } catch (...) {
            DH_LOG_W("R0 getMaterialInfo(%s) threw", probe);
        }
    }
    DH_LOG_I("R0 switchable() group: %zu materials", mce::RenderMaterialGroup::switchable().mMaterials.get().size());
    int         part = 0;
    for (auto const& kv : mats) {
        auto const& name = kv.first.getString();
        hasRC |= name == "RenderChunk";
        hasFB |= name == "MeshFallbackPosUVNormalColor";
        if (line.size() + name.size() > 700) {
            DH_LOG_I("R0 materials[%d]: %s", part++, line.c_str());
            line.clear();
        }
        if (!line.empty()) line += ',';
        line += name;
    }
    if (!line.empty()) DH_LOG_I("R0 materials[%d]: %s", part++, line.c_str());
    DH_LOG_I(
        "R0 materials: count=%zu hasRenderChunk=%d hasMeshFallbackPosUVNormalColor=%d => %s", mats.size(), hasRC ? 1 : 0, hasFB ? 1 : 0,
        (mats.size() >= 150 && hasRC && hasFB) ? "PASS" : "FAIL"
    );
    auto* lr = ci.getLevelRenderer();
    DH_LOG_I(
        "R0 selectionOpaqueMaterial valid=%d fallback valid=%d", (lr && lr->mLevelRendererPlayer) ? materialValid(lr->mLevelRendererPlayer->selectionOpaqueMaterial.get()) : -1,
        materialValid(fallbackMaterial()) ? 1 : 0
    );
    DH_LOG_I("R0 ===== inventory end =====");
}

// R0 + R1 + R1b + R3(b): LevelRendererCamera::render, after the world has been rendered.
LL_TYPE_INSTANCE_HOOK(
    SpikeRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$render,
    void,
    ::BaseActorRenderContext& rc,
    ::ViewRenderObject const& vro,
    ::IClientInstance&        ci
) {
    origin(rc, vro, ci);
    if (vro.mViewData->mIsShadowPass) return;
    auto& ctx = rc.mScreenContext;
    if (gDumpR0.exchange(false)) dumpInventory(ctx, vro, *this, ci);
    glm::dvec3 camPos{vro.mViewData->mCameraPos->x, vro.mViewData->mCameraPos->y, vro.mViewData->mCameraPos->z};
    drawSpikes(ctx, ci.getLevelRenderer(), camPos, "LevelRendererCamera::render", true, 1.0f, 0.0f, 1.0f);
}

glm::dvec3 cameraPosOf(ScreenContext& ctx) {
    mce::MeshContext& mctx = ctx;
    auto&             p    = mctx.camera.mPosition.get();
    return {p.x, p.y, p.z};
}

// R2a: LevelRenderer::renderLevel (green).
LL_TYPE_INSTANCE_HOOK(
    SpikeRenderLevelHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::renderLevel,
    void,
    ::ScreenContext&            sc,
    ::FrameRenderObject const&  fro
) {
    origin(sc, fro);
    drawSpikes(sc, this, cameraPosOf(sc), "LevelRenderer::renderLevel", false, 0.0f, 1.0f, 0.0f);
}

// R2b: InGamePlayScreen::_postLevelRender (yellow).
LL_TYPE_INSTANCE_HOOK(
    SpikePostLevelHook,
    ll::memory::HookPriority::Normal,
    InGamePlayScreen,
    &InGamePlayScreen::$_postLevelRender,
    void,
    ::ScreenContext& sc,
    ::LevelRenderer& lr
) {
    origin(sc, lr);
    drawSpikes(sc, &lr, cameraPosOf(sc), "InGamePlayScreen::_postLevelRender", false, 1.0f, 1.0f, 0.0f);
}

// R3(a): after setupCamera, override mZFar and rebuild the projection top.
LL_TYPE_INSTANCE_HOOK(
    SpikeSetupCameraHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,
    void,
    ::mce::Camera& cam,
    float          a
) {
    origin(cam, a);
    if (gR3 != 2) return;
    float     oldFar = cam.mZFar;
    glm::mat4 p      = cam.projectionMatrixStack->top()._m.get();
    auto      info   = analyzeProjection(p, cam.mZNear, oldFar);
    static int logged = 0;
    if (!info.valid) {
        if (logged++ < 3) DH_LOG_W("R3a setupCamera: projection layout not recognised, mZFar override skipped");
        return;
    }
    cam.mZFar = 20000.0f;
    cam.projectionMatrixStack->getTop()._m = patchFar(p, info, cam.mZNear, oldFar, 20000.0f);
    cam.updateViewMatrixDependencies();
    if (logged++ < 3) {
        auto ci = ll::service::getClientInstance();
        DH_LOG_I(
            "R3a setupCamera: cam==ci.camera:%d mZFar %.1f -> %.1f (%s) near=%.3f", (ci && &ci->getCamera() == &cam) ? 1 : 0, oldFar,
            cam.mZFar, info.kind, cam.mZNear
        );
    }
}

void onKeys(ll::event::AfterUIRenderEvent& e) {
    if (e.screenView().getScreenName() != "hud_screen") return;
    // F11 is Bedrock's fullscreen toggle (GDK build) and is not used here.
    bool shift = dh::input::shiftDown();
    bool ctrl  = dh::input::ctrlDown();
    if (dh::input::pressed(VK_F9)) {
        gDumpR0 = true;
        DH_LOG_I("R0: dump requested (F9); it runs on the next world render");
    }
    if (dh::input::pressed(VK_F10)) {
        if (shift) {
            auto& s = settings();
            s.origin = s.origin == OriginMode::CameraRelative ? OriginMode::Absolute : OriginMode::CameraRelative;
            DH_LOG_I("spike origin mode -> %s", s.origin == OriginMode::CameraRelative ? "camera-relative" : "absolute");
        } else if (ctrl) {
            gR1b = !gR1b;
            if (gR1b) gAnchor.set = false;
            DH_LOG_I("R1b: %s (Ctrl+F10)", gR1b ? "on" : "off");
        } else {
            gR1 = !gR1;
            if (gR1) gAnchor.set = false;
            DH_LOG_I("R1/R2: %s (F10)", gR1 ? "on" : "off");
        }
    }
    if (dh::input::pressed(VK_F12)) {
        if (shift) {
            auto& s = settings();
            s.prim  = s.prim == PrimMode::QuadList ? PrimMode::TriangleList : PrimMode::QuadList;
            resetMeshes();
            DH_LOG_I("spike primitive mode -> %s (Shift+F12)", s.prim == PrimMode::QuadList ? "QuadList" : "TriangleList");
        } else {
            gR3 = (gR3 + 1) % 3;
            if (gR3 > 0) gAnchor.set = false;
            DH_LOG_I("R3 mode -> %s (F12)", r3Name(gR3));
        }
    }
    updateHud();
}

} // namespace

void install() {
    if (gInstalled) return;
    gInstalled = true;
    SpikeRenderHook::hook();
    SpikeRenderLevelHook::hook();
    SpikePostLevelHook::hook();
    SpikeSetupCameraHook::hook();
    auto& bus     = ll::event::EventBus::getInstance();
    gKeyListener  = bus.emplaceListener<ll::event::AfterUIRenderEvent>(onKeys);
    gExitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
        resetMeshes();
        gAnchor.set = false;
    });
    updateHud();
    DH_LOG_I("spikes installed (all off; F9 R0 dump, F10 R1/R2, Ctrl+F10 R1b, F12 R3, Shift+F10 origin, Shift+F12 primitive)");
}

void uninstall() {
    if (!gInstalled) return;
    gInstalled = false;
    gR1 = gR1b = false;
    gR3 = 0;
    auto& bus = ll::event::EventBus::getInstance();
    if (gKeyListener) bus.removeListener(gKeyListener);
    if (gExitListener) bus.removeListener(gExitListener);
    gKeyListener.reset();
    gExitListener.reset();
    SpikeSetupCameraHook::unhook();
    SpikePostLevelHook::unhook();
    SpikeRenderLevelHook::unhook();
    SpikeRenderHook::unhook();
    resetMeshes();
}

} // namespace dh::spikes
