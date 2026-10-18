#pragma once

// Stage B (spike B3): serves our custom DH_Terrain.material.bin through the game's own resource loader.
//   * ResourcePackManager::$load(ResourceLocation const&, std::string&):
//       renderer/materials/materials.index.json   -> origin(), then a {"name":"DH_Terrain","path":"DH_Terrain"} entry
//       is spliced in renderer/materials/DH_Terrain.material.bin -> contents of
//       <modDir>/materials/DH_Terrain.material.bin, returns true
//   * RenderMaterialGroup::loadList: probe, logs the list path and whether "DH_Terrain" resolves afterwards.
// Toggle: env DH_MATERIAL_INJECT=0|1 (default: on iff the .material.bin exists). See docs/spikes/stage-b-offline.md.

namespace dh::render {

void install();
void uninstall();

} // namespace dh::render
