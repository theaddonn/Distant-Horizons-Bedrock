#pragma once

// Shared helpers for the render spikes R0-R3: vertex-format probe, spike mesh cache, world-space draw and projection
// analysis. Everything here runs on the render thread.

#include <glm/glm.hpp>

#include <string>

class ScreenContext;
class Matrix;
namespace mce {
class Camera;
class MaterialPtr;
} // namespace mce

namespace dh::spikes {

enum class OriginMode : int { CameraRelative = 0, Absolute = 1 };
enum class PrimMode : int { QuadList = 0, TriangleList = 1 };

struct DrawSettings {
    OriginMode origin = OriginMode::CameraRelative;
    PrimMode   prim   = PrimMode::QuadList;
};
DrawSettings& settings();

/// Vertical double-sided quad. `axisZ`: quad lies in the XY plane (faces +-Z) if true, in the ZY plane otherwise.
struct QuadSpec {
    bool  axisZ  = true;
    float width  = 10.0f;
    float height = 10.0f;
    float r = 1, g = 0, b = 1;
};

/// Builds (or reuses) the mesh and draws it with its base-centre at `world` (x, baseY, z). Returns false and logs on
/// failure. `material` must be valid (see materialValid).
bool drawQuad(
    ScreenContext&          ctx,
    QuadSpec const&         quad,
    mce::MaterialPtr const& material,
    glm::dvec3 const&       world,
    glm::dvec3 const&       cameraPos,
    char const*             tag,
    float                   localFar = 0.0f /* >0: push a projection with this far plane around the draw */
);

bool materialValid(mce::MaterialPtr const& m);

/// Fallback material `MeshFallbackPosUVNormalColor`; may be invalid.
mce::MaterialPtr const& fallbackMaterial();
/// Stage-B `DH_Terrain` (served by the MaterialInjector, looked up in switchable()); invalid until the injector's
/// index splice has been loaded. Re-resolved whenever invalid, like fallbackMaterial().
mce::MaterialPtr const& dhMaterial(mce::MaterialPtr const* altSource = nullptr);

/// Drops cached meshes (call when the buffer service changes or on level exit).
void resetMeshes();

/// Detected projection layout.
struct ProjInfo {
    bool  valid      = false;
    bool  transposed = false; // true: row-vector convention (v * M), false: column-vector (M * v)
    float eyeZNear   = 0;     // eye-space z of the near plane that gives w = +near
    float eyeZFar    = 0;
    float ndcNear    = 0;
    float ndcFar     = 0;
    float wNear      = 0;
    char  kind[64]{};
};

ProjInfo analyzeProjection(glm::mat4 const& p, float zNear, float zFar);

/// Returns `p` with its far plane moved to `newFar`, preserving the detected convention and near plane.
glm::mat4 patchFar(glm::mat4 const& p, ProjInfo const& info, float zNear, float zFar, float newFar);

/// Logs the native vertex format seen by a one-vertex Tessellator probe (R0).
void logProbe(ScreenContext& ctx);

/// Logs the 16 floats and the NDC analysis for the camera's current projection (R3 raw numbers).
void logProjection(char const* tag, mce::Camera& cam);

} // namespace dh::spikes
