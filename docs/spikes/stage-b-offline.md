# Stage B offline artifacts: user checklist

Everything here was built offline, **untested in game**. Rebuild: `tools/shaders/build_stage_b.sh` (needs `build_mbt.sh`, unpacked vanilla materials in
`~/.cache/dh-tools/unpacked/`, dxc, wine+fxc_lite; see `shader-toolchain.md` section 9). Outputs (gitignored): `artifacts/shaders/B2/`, `artifacts/shaders/B3/`.

## B2: tinted RenderChunk (does the engine accept a modified vanilla material, and does our FS run?)
1. Quit the game. `cd <game>/data/renderer/materials`; `cp RenderChunk.material.bin RenderChunk.material.bin.orig`.
2. `cp <repo>/artifacts/shaders/B2/RenderChunk.material.bin .`; launch, load a world.
3. PASS: opaque terrain is red/pink, textures still visible (water, glass, leaves unaffected). FAIL variants to report: black/white/missing terrain
   (interface or sampler mismatch), crash on startup or vanilla look (file rejected: hash/integrity), or only some blocks tinted (variant coverage).
4. Restore: `cp RenderChunk.material.bin.orig RenderChunk.material.bin`.

## B3: DH_Terrain loaded through the mod (no game-dir writes)
1. `scripts/build.sh client && scripts/deploy.sh client` (copies `artifacts/shaders/B3/DH_Terrain.material.bin` to `<modDir>/materials/`).
   Toggle with env `DH_MATERIAL_INJECT=0|1` (default on iff the file exists). Launch the game, then read `<modDir>/logs/dh.log`:
   - `materials: injector on, DH_Terrain.material.bin N bytes` (at enable)
   - `materials: load renderer/materials/...` (up to 40 trace lines of every material path loaded; tells us which paths hit our hook)
   - `materials: materials.index.json spliced (A -> B bytes)`  -> the index load goes through `ResourcePackManager::$load(loc, string&)`
   - `materials: DH_Terrain served N bytes`                   -> the loader asked for our file
   - `materials: loadList(<path>, clean=..) -> 1; DH_Terrain resolved: yes|no`  -> `getMaterialInfo("DH_Terrain")` has a compiled material
2. Interpretation: no `spliced` line -> index is loaded by another overload/path (check `load(ext)` trace lines) or from a cache; `spliced` but no `served`
   -> loader skipped the entry (e.g. path/extension mismatch); `served` but `resolved: no` -> parse/validation failure (hash, version, signature).
3. If `resolved: yes`: use material `DH_Terrain` (via `MaterialPtr{common(), "DH_Terrain"}`) for the F10 quad of the Stage-A spike renderer; expected look: vertex colour,
   per-chunk greenish tint if `u_model[0][3]` carries the origin (a flat colour means the smuggled value is constant/0).

## Open questions / risks
- Is the materials index/`.material.bin` read via `ResourcePackManager::$load(ResourceLocation const&, std::string&)` at all (vs. the extension-list overload, `loadText`, or a preloaded cache)? The trace lines answer this.
- Does the loader check `sourceHash`, `inputHash`, `passExtra`, encryption, or file integrity of `data/renderer/materials`? (B2 answers for modified vanilla.)
- `mul` conventions in `dh_terrain.vert.hlsl` (`mul(u_proj, mul(u_view, mul(u_model[0], v)))` with column_major) were not verified against vanilla DXIL; wrong transposition = garbage geometry.
- FS UseTextures/Lit flag variants all get the same non-sampling FS; MatTexture binding is irrelevant for B3.
- SM40/50 are dropped from DH_Terrain (and the B2 SM50/40 FS come from wine's fxc, `SHDR` not `SHEX`); only relevant if the game runs D3D11.
- `DhParams` is only settable if the material path exposes property fields for our draw; the default (1,1,1,1) is what B3 shows without that.
- MaterialInjector is process-wide: if the game updates and the loader signature moves, the hooks fail to resolve (build break or no-op) - check the log for `injector on`.
