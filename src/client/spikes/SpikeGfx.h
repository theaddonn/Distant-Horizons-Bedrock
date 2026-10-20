#pragma once

// Render helpers shared with the LOD renderer: the DH_Terrain* material lookups and the projection analysis / far-plane
// patch. Everything here runs on the render thread. (The R0-R3 spike module that used to live next to it is gone.)

#include <glm/glm.hpp>

#include <string>

namespace mce {
class MaterialPtr;
} // namespace mce

namespace dh::spikes {

bool materialValid(mce::MaterialPtr const& m);

/// Fallback material `MeshFallbackPosUVNormalColor`; may be invalid.
mce::MaterialPtr const& fallbackMaterial();
/// Stage-B `DH_Terrain` (served by the MaterialInjector, looked up in switchable()); invalid until the injector's
/// index splice has been loaded. Re-resolved whenever invalid, like fallbackMaterial().
mce::MaterialPtr const& dhMaterial(mce::MaterialPtr const* altSource = nullptr);
/// `DH_TerrainTransparent`: same compiled shader, blended render state (depth test, no depth write). May be invalid.
mce::MaterialPtr const& dhTransparentMaterial();
/// `DH_TerrainLate`: same compiled shader, opaque output and depth write, but flagged Blending so the dragon frame
/// graph draws it in the Transparent pass, after the stars / sun / moon (Run 13). May be invalid.
mce::MaterialPtr const& dhLateMaterial();
/// `DH_TerrainWater`: blended like DH_TerrainTransparent but with depth write, for the LOD translucent layer when the
/// opaque layer is repeated in the Transparent pass (SkyOrder::Overdraw, Run 14). May be invalid.
mce::MaterialPtr const& dhWaterMaterial();

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

} // namespace dh::spikes
