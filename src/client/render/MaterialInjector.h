#pragma once

// Stage B (spike B3): serves our custom DH_Terrain.material.bin through the game's own resource loader.
//   * ResourcePackManager::$load(ResourceLocation const&, std::string&):
//       renderer/materials/materials.index.json   -> origin(), then a {"name":"DH_Terrain","path":"DH_Terrain"} entry
//       is spliced in renderer/materials/DH_Terrain.material.bin -> contents of
//       <modDir>/materials/DH_Terrain.material.bin, returns true
//   * RenderMaterialGroup::loadList: probe, logs the list path and whether "DH_Terrain" resolves afterwards.
// Toggle: env DH_MATERIAL_INJECT=0|1 (default: on iff the .material.bin exists). See docs/spikes/stage-b-offline.md.

#include <cstdint>

namespace mce {
class MaterialPtr;
}

namespace dh::render {

void installMaterialInjector();
void uninstallMaterialInjector();

/// Render thread, once `fallback` (MeshFallbackPosUVNormalColor) is valid: clones its mce::RenderMaterial into the
/// "DH_Terrain" info so the dragon binder can substitute our compiled material. False until possible / if no binder.
bool ensureDhMceMaterial(mce::MaterialPtr const& fallback, char const* sourceName = "?");

/// Cumulative dragon binding resolves of the four DH clones (0 DH_Terrain, 1 DH_TerrainTransparent, 2 DH_TerrainLate,
/// 3 DH_TerrainWater), counted in the resolve hook (any thread). The 2026-10-20 rate (~10k/s) suggests one resolve per
/// submitted draw (unverified); the `render: perf` line logs the per-10 s delta next to the CPU-side call counts.
void dhBindingResolves(std::uint64_t out[4]);

} // namespace dh::render
