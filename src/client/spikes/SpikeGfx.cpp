#include "client/spikes/SpikeGfx.h"

#include "client/render/MaterialInjector.h"

#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace dh::spikes {

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

mce::MaterialPtr const& dhTransparentMaterial() {
    // Cloned together with DH_Terrain by dhMaterial()/ensureDhMceMaterial; only the lookup lives here.
    static std::optional<mce::MaterialPtr> mat;
    if (!mat || !materialValid(*mat))
        mat.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"DH_TerrainTransparent"});
    return *mat;
}

mce::MaterialPtr const& dhLateMaterial() {
    static std::optional<mce::MaterialPtr> mat;
    if (!mat || !materialValid(*mat))
        mat.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"DH_TerrainLate"});
    return *mat;
}

mce::MaterialPtr const& dhWaterMaterial() {
    static std::optional<mce::MaterialPtr> mat;
    if (!mat || !materialValid(*mat))
        mat.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"DH_TerrainWater"});
    return *mat;
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

} // namespace dh::spikes
