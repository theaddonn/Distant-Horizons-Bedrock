#include "client/spikes/SpikeGfx.h"

#include "bedrock_common/FileLogger.h"
#include "client/render/MaterialInjector.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshContext.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/renderer/ShaderColor.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace dh::spikes {

namespace {

using Texture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;

std::map<std::string, std::unique_ptr<mce::Mesh>> gMeshes;
void*                                             gService = nullptr;
std::set<std::string>                             gLogged;

struct Probe {
    bool                 ok = false;
    std::array<bool, 15> fields{};
    std::string          sizes;
};

template <class V>
void replicate(V& v, size_t n) {
    if (v.size() == 1) v.assign(n, v.front());
}

mce::MeshData buildMeshData(ScreenContext& ctx, QuadSpec const& q, PrimMode prim, Probe& probe) {
    auto primitive = prim == PrimMode::QuadList ? mce::PrimitiveMode::QuadList : mce::PrimitiveMode::TriangleList;

    // One-vertex Tessellator probe: yields the native vertex format (mFieldEnabled) and the packed colour for
    // exactly the colour we want, without hard-coding the packing.
    Tessellator seed{ctx.tessellator.mBufferResourceService};
    seed.resetTransform(true);
    seed.begin({}, primitive, 4, false);
    seed.color(q.r, q.g, q.b, 1.0f);
    seed.tex2(Vec2{0.5f, 0.5f});
    seed.mNextNormal = glm::vec4{q.axisZ ? 0.f : 1.f, 0.f, q.axisZ ? 1.f : 0.f, 0.f};
    seed.vertex(0.f, 0.f, 0.f);

    mce::MeshData md(seed.mMeshData.get());
    probe.ok     = true;
    probe.fields = md.mFieldEnabled.get();
    {
        char b[256];
        snprintf(
            b,
            sizeof(b),
            "pos=%zu nrm=%zu tan=%zu col=%zu uv0=%zu uv1=%zu uv2=%zu idx=%zu",
            md.mPositions->size(),
            md.mNormals->size(),
            md.mTangents->size(),
            md.mColors->size(),
            md.mTextureUVs[0]->size(),
            md.mTextureUVs[1]->size(),
            md.mTextureUVs[2]->size(),
            md.mIndices->size()
        );
        probe.sizes = b;
    }

    float     hw = q.width * 0.5f;
    auto      P  = [&](float u, float y) { return q.axisZ ? glm::vec3{u, y, 0.f} : glm::vec3{0.f, y, u}; };
    glm::vec3 c0 = P(-hw, 0), c1 = P(hw, 0), c2 = P(hw, q.height), c3 = P(-hw, q.height);

    std::vector<glm::vec3> pos;
    if (prim == PrimMode::QuadList) {
        pos = {c0, c1, c2, c3, c3, c2, c1, c0}; // front + reversed winding (no assumption about culling)
    } else {
        pos = {c0, c1, c2, c0, c2, c3, c0, c2, c1, c0, c3, c2};
    }
    size_t n = pos.size();

    uint c        = md.mColors->empty() ? 0xffff00ffu : md.mColors->front();
    md.mPositions = pos;
    if (!md.mColors->empty() || probe.fields[0]) md.mColors = std::vector<uint>(n, c);
    if (md.mTextureUVs[0]->size() == 1) {
        std::vector<glm::vec2> uv(n);
        for (size_t i = 0; i < n; ++i)
            uv[i] = glm::vec2{(i % 4 == 1 || i % 4 == 2) ? 1.f : 0.f, (i % 4 >= 2) ? 1.f : 0.f};
        md.mTextureUVs[0] = uv;
    }
    replicate(md.mNormals.get(), n);
    replicate(md.mTangents.get(), n);
    replicate(md.mBoneId0s.get(), n);
    replicate(md.mTextureUVs[1].get(), n);
    replicate(md.mTextureUVs[2].get(), n);
    replicate(md.mPBRTextureIndices.get(), n);
    replicate(md.mMERS.get(), n);
    replicate(md.mGeoType.get(), n);
    md.mAABB = std::pair{
        glm::vec3{-hw, 0,        -hw},
        glm::vec3{hw,  q.height, hw }
    };
    md.mUVAABB = std::pair{glm::vec2{0}, glm::vec2{1}};
    return md;
}

mce::Mesh* getMesh(ScreenContext& ctx, QuadSpec const& q, PrimMode prim) {
    auto svc = ctx.tessellator.mBufferResourceService.lock();
    if (!svc) return nullptr;
    if (gService != svc.get()) {
        resetMeshes();
        gService = svc.get();
    }
    char key[128];
    snprintf(
        key,
        sizeof(key),
        "%d|%.1f|%.1f|%.2f,%.2f,%.2f|%d",
        q.axisZ,
        q.width,
        q.height,
        q.r,
        q.g,
        q.b,
        static_cast<int>(prim)
    );
    auto it = gMeshes.find(key);
    if (it != gMeshes.end()) return it->second.get();

    Probe       probe;
    auto        md    = buildMeshData(ctx, q, prim, probe);
    auto        count = md.mPositions->size();
    auto        mesh  = std::make_unique<mce::Mesh>(svc, std::move(md), false, "dh_spike");
    std::string f;
    for (bool b : probe.fields) f += b ? '1' : '0';
    DH_LOG_I("spike mesh built key=%s verts=%zu fieldEnabled=%s probe[%s]", key, count, f.c_str(), probe.sizes.c_str());
    auto* raw    = mesh.get();
    gMeshes[key] = std::move(mesh);
    return raw;
}

bool drawInner(
    ScreenContext&          ctx,
    mce::Mesh&              mesh,
    mce::MaterialPtr const& material,
    glm::vec3               t,
    char const*             tag,
    size_t                  verts
) {
    mce::MeshContext& mctx = ctx;
    auto&             cam  = mctx.camera;
    auto              wref = cam.worldMatrixStack->push(false);
    Matrix            m    = Matrix::IDENTITY();
    m.translate(t.x, t.y, t.z);
    cam.worldMatrixStack->getTop() = m;

    auto&      sc    = mctx.currentShaderColor;
    mce::Color saved = sc.color.get();
    sc.color         = mce::Color{1.f, 1.f, 1.f, 1.f};
    sc.dirty         = true;

    static Texture const none{std::monostate{}};
    mesh.renderMesh(mctx, material, none, 0, static_cast<uint>(verts), OffscreenCaptureDescription{}, nullptr);

    sc.color = saved;
    sc.dirty = true;

    std::string k = std::string("draw:") + tag;
    if (gLogged.insert(k).second) {
        DH_LOG_I("spike draw [%s] ok verts=%zu translate=(%.2f,%.2f,%.2f)", tag, verts, t.x, t.y, t.z);
    }
    return true;
}

} // namespace

DrawSettings& settings() {
    static DrawSettings s;
    return s;
}

void logProbe(ScreenContext& ctx) {
    for (auto prim : {PrimMode::QuadList, PrimMode::TriangleList}) {
        Probe       probe;
        QuadSpec    q;
        auto        md = buildMeshData(ctx, q, prim, probe);
        std::string f;
        for (bool b : probe.fields) f += b ? '1' : '0';
        DH_LOG_I(
            "R0 tessellator probe prim=%s: mFieldEnabled=%s colour=0x%08x streams[%s]",
            prim == PrimMode::QuadList ? "QuadList" : "TriangleList",
            f.c_str(),
            md.mColors->empty() ? 0u : md.mColors->front(),
            probe.sizes.c_str()
        );
    }
}

bool materialValid(mce::MaterialPtr const& m) {
    return m.mRenderMaterialInfoPtr && m.mRenderMaterialInfoPtr->mPtr.get() != nullptr;
}

mce::MaterialPtr const& fallbackMaterial() {
    // Re-resolved whenever it is invalid: the pointer resolves to nothing before the material group has loaded the
    // list, and the group's entries are replaced on a clean reload (world exit / video settings), which left the
    // old static dangling ("material invalid" on R1b after leaving a world, 2026-10-19).
    // R0 (2026-10-19): common() holds the 137 legacy-named materials (ui_*, particles_*, ...); the compiled
    // materials.index.json entries (1014, incl. MeshFallbackPosUVNormalColor, RenderChunk) live in switchable().
    static std::optional<mce::MaterialPtr> mat;
    if (!mat || !materialValid(*mat)) {
        mat.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"MeshFallbackPosUVNormalColor"});
        if (!materialValid(*mat)) {
            mat.emplace(mce::RenderMaterialGroup::common(), HashedString{"MeshFallbackPosUVNormalColor"});
        }
    }
    return *mat;
}

mce::MaterialPtr const& dhMaterial(mce::MaterialPtr const* altSource) {
    static std::optional<mce::MaterialPtr> mat;
    if (!mat || !materialValid(*mat)) {
        // Clone source: MeshFallbackPosUVNormalColor when the engine has loaded it (lazily, minutes into a session),
        // otherwise any valid mce material the caller offers (selectionOpaqueMaterial is valid from the first frame;
        // its legacy shaders map to the same MeshFallback* dragon family, and the request is renamed anyway).
        // Order matters for the render STATE the clone inherits (depth test/write, blending): selectionOpaqueMaterial
        // is an overlay (2026-10-19: far LOD sections drew over near ones). entity_static (legacy, common()) is a
        // plain depth-tested opaque material with entity.vertex -> the MeshFallback dragon family, like the others.
        static std::optional<mce::MaterialPtr> entityStatic;
        if (!entityStatic || !materialValid(*entityStatic))
            entityStatic.emplace(mce::RenderMaterialGroup::common(), HashedString{"entity_static"});
        if (materialValid(fallbackMaterial()))
            dh::render::ensureDhMceMaterial(fallbackMaterial(), "MeshFallbackPosUVNormalColor");
        else if (materialValid(*entityStatic)) dh::render::ensureDhMceMaterial(*entityStatic, "entity_static");
        else if (altSource && materialValid(*altSource))
            dh::render::ensureDhMceMaterial(*altSource, "selectionOpaqueMaterial");
        mat.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"DH_Terrain"});
    }
    return *mat;
}

void resetMeshes() {
    for (auto& [k, v] : gMeshes) (void)v.release(); // leak on purpose: the buffer service may already be gone
    gMeshes.clear();
    gLogged.clear();
    gService = nullptr;
}

bool drawQuad(
    ScreenContext&          ctx,
    QuadSpec const&         quad,
    mce::MaterialPtr const& material,
    glm::dvec3 const&       world,
    glm::dvec3 const&       cameraPos,
    char const*             tag,
    float                   localFar
) {
    if (!materialValid(material)) {
        std::string k = std::string("badmat:") + tag;
        if (gLogged.insert(k).second) DH_LOG_W("spike draw [%s] skipped: material invalid", tag);
        return false;
    }
    auto  prim = settings().prim;
    auto* mesh = getMesh(ctx, quad, prim);
    if (!mesh) return false;
    size_t verts = prim == PrimMode::QuadList ? 8 : 12;

    glm::vec3 t = settings().origin == OriginMode::CameraRelative ? glm::vec3(world - cameraPos) : glm::vec3(world);

    if (localFar > 0.0f) {
        mce::MeshContext& mctx = ctx;
        auto&             cam  = mctx.camera;
        auto              pref = cam.projectionMatrixStack->push(false);
        glm::mat4         p    = cam.projectionMatrixStack->top()._m.get();
        auto              info = analyzeProjection(p, cam.mZNear, cam.mZFar);
        if (info.valid) cam.projectionMatrixStack->getTop()._m = patchFar(p, info, cam.mZNear, cam.mZFar, localFar);
        else DH_LOG_W("spike [%s]: projection layout not recognised, drawing with the vanilla projection", tag);
        return drawInner(ctx, *mesh, material, t, tag, verts);
    }
    return drawInner(ctx, *mesh, material, t, tag, verts);
}

ProjInfo analyzeProjection(glm::mat4 const& p, float zNear, float zFar) {
    // R0 (2026-10-19): cam.mZNear/mZFar read as 0 on this build, so near/far are derived from the matrix itself:
    // ndc(z) = (A z + B) / (Wz z) is 0 at z0 = -B/A and 1 at z1 = -B/(A - Wz); the one nearer the eye is the near
    // plane. A missing/unusable root means an infinite far plane (ndcFar = A/Wz). The arguments are only a fallback.
    ProjInfo best;
    for (int layout = 0; layout < 2; ++layout) {
        bool  tr = layout == 1;
        float A  = p[2][2];
        float B  = tr ? p[2][3] : p[3][2];
        float Wz = tr ? p[3][2] : p[2][3];
        float Ww = p[3][3];
        if (std::fabs(std::fabs(Wz) - 1.0f) > 1e-3f || std::fabs(Ww) > 1e-3f) continue;
        ProjInfo r;
        r.valid       = true;
        r.transposed  = tr;
        float cand[2] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
        if (std::fabs(A) > 1e-12f) cand[0] = -B / A;
        if (std::fabs(A - Wz) > 1e-12f) cand[1] = -B / (A - Wz);
        float nearZ = std::numeric_limits<float>::quiet_NaN(), farZ = std::numeric_limits<float>::quiet_NaN();
        for (float z : cand) {
            if (!std::isfinite(z) || Wz * z <= 0.0f) continue; // behind the eye / not a plane
            if (!std::isfinite(nearZ) || std::fabs(z) < std::fabs(nearZ)) {
                farZ  = nearZ;
                nearZ = z;
            } else if (!std::isfinite(farZ) || std::fabs(z) > std::fabs(farZ)) {
                farZ = z;
            }
        }
        if (!std::isfinite(nearZ) && zNear > 0.0f) nearZ = Wz > 0 ? zNear : -zNear; // fallback: trust the caller
        if (!std::isfinite(nearZ)) continue;
        const bool infiniteFar = !std::isfinite(farZ);
        if (infiniteFar)
            farZ = Wz > 0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        (void)zFar;
        r.eyeZNear        = nearZ;
        r.eyeZFar         = farZ;
        r.wNear           = Wz * nearZ + Ww;
        r.ndcNear         = (A * nearZ + B) / r.wNear;
        r.ndcFar          = infiniteFar ? A / Wz : (A * farZ + B) / (Wz * farZ + Ww);
        auto        close = [](float a, float b) { return std::fabs(a - b) < 0.02f; };
        char const* k     = "unknown";
        if (close(r.ndcNear, 0) && close(r.ndcFar, 1)) k = "ZO 0..1";
        else if (close(r.ndcNear, 1) && close(r.ndcFar, 0)) k = "ZO reversed 1..0";
        else if (close(r.ndcNear, -1) && close(r.ndcFar, 1)) k = "NO -1..1";
        else if (close(r.ndcNear, 1) && close(r.ndcFar, -1)) k = "NO reversed 1..-1";
        snprintf(
            r.kind,
            sizeof(r.kind),
            "%s, %s-handed, near=%.3f far=%s%.0f",
            k,
            Wz > 0 ? "left" : "right",
            std::fabs(nearZ),
            infiniteFar ? "inf " : "",
            infiniteFar ? 0.0f : std::fabs(farZ)
        );
        if (!best.valid) best = r;
    }
    return best;
}

glm::mat4 patchFar(glm::mat4 const& p, ProjInfo const& info, float zNear, float zFar, float newFar) {
    (void)zFar;
    auto  snap = [](float v) { return std::round(v); };
    float tN = snap(info.ndcNear), tF = snap(info.ndcFar);
    bool  tr = info.transposed;
    float Wz = tr ? p[3][2] : p[2][3];
    float Ww = p[3][3];
    float zn = info.eyeZNear;
    float zf = Wz > 0 ? newFar : -newFar;
    float wn = Wz * zn + Ww, wf = Wz * zf + Ww;
    float A = (tF * wf - tN * wn) / (zf - zn);
    float B = tN * wn - A * zn;
    (void)zNear;
    glm::mat4 out = p;
    out[2][2]     = A;
    if (tr) out[2][3] = B;
    else out[3][2] = B;
    return out;
}

namespace {
std::string mat16(glm::mat4 const& m) {
    char b[400];
    snprintf(
        b,
        sizeof(b),
        "[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]",
        m[0][0],
        m[0][1],
        m[0][2],
        m[0][3],
        m[1][0],
        m[1][1],
        m[1][2],
        m[1][3],
        m[2][0],
        m[2][1],
        m[2][2],
        m[2][3],
        m[3][0],
        m[3][1],
        m[3][2],
        m[3][3]
    );
    return b;
}

void refNdc(char const* name, glm::mat4 const& m, float eyeNear, float eyeFar) {
    glm::vec4 a = m * glm::vec4(0, 0, eyeNear, 1), b = m * glm::vec4(0, 0, eyeFar, 1);
    DH_LOG_I("R3 ref %s: ndcz(near)=%.6f ndcz(far)=%.6f", name, a.z / a.w, b.z / b.w);
}
} // namespace

void logProjection(char const* tag, mce::Camera& cam) {
    glm::mat4 p = cam.projectionMatrixStack->top()._m.get();
    glm::mat4 v = cam.viewMatrixStack->top()._m.get();
    glm::mat4 w = cam.worldMatrixStack->top()._m.get();
    float     n = cam.mZNear, f = cam.mZFar;
    DH_LOG_I(
        "%s camera: zNear=%.6f zFar=%.6f fov=%.6f aspect=%.6f pos=(%.3f,%.3f,%.3f)",
        tag,
        n,
        f,
        cam.mFov,
        cam.mAspectRatio,
        cam.mPosition->x,
        cam.mPosition->y,
        cam.mPosition->z
    );
    DH_LOG_I("%s proj (glm m[col][row], printed col by col) %s", tag, mat16(p).c_str());
    DH_LOG_I("%s view top %s", tag, mat16(v).c_str());
    DH_LOG_I("%s world top %s", tag, mat16(w).c_str());
    for (int layout = 0; layout < 2; ++layout) {
        bool  tr = layout == 1;
        float A = p[2][2], B = tr ? p[2][3] : p[3][2], Wz = tr ? p[3][2] : p[2][3], Ww = p[3][3];
        for (int s = -1; s <= 1; s += 2) {
            float zn = static_cast<float>(s) * n, zf = static_cast<float>(s) * f;
            float wn = Wz * zn + Ww, wf = Wz * zf + Ww;
            DH_LOG_I(
                "R3 layout=%s eyeSign=%+d: ndcz(near)=%.6f (w=%.4f) ndcz(far)=%.6f (w=%.4f)",
                tr ? "row-vector" : "column-vector",
                s,
                (A * zn + B) / wn,
                wn,
                (A * zf + B) / wf,
                wf
            );
        }
    }
    auto info = analyzeProjection(p, n, f);
    if (info.valid) {
        DH_LOG_I(
            "R3 detected: layout=%s eyeZnear=%.4f ndcNear=%.6f ndcFar=%.6f kind=%s",
            info.transposed ? "row-vector" : "column-vector",
            info.eyeZNear,
            info.ndcNear,
            info.ndcFar,
            info.kind
        );
        auto patched = patchFar(p, info, n, f, 20000.0f);
        DH_LOG_I("R3 patchFar(20000) -> %s", mat16(patched).c_str());
    } else {
        DH_LOG_W("R3 detected: projection layout not recognised (no |w=+-z| layout)");
    }
    float fov = cam.mFov > 3.2f ? glm::radians(cam.mFov) : cam.mFov;
    refNdc("glm RH_ZO", glm::perspectiveRH_ZO(fov, cam.mAspectRatio, n, f), -n, -f);
    refNdc("glm LH_ZO", glm::perspectiveLH_ZO(fov, cam.mAspectRatio, n, f), n, f);
    refNdc("glm RH_NO", glm::perspectiveRH_NO(fov, cam.mAspectRatio, n, f), -n, -f);
    refNdc("glm LH_NO", glm::perspectiveLH_NO(fov, cam.mAspectRatio, n, f), n, f);
}

} // namespace dh::spikes
