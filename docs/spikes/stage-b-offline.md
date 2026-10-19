# Stage B offline artifacts: user checklist

Everything here was built offline, **untested in game**. Rebuild: `tools/shaders/build_stage_b.sh` (needs `build_mbt.sh`, unpacked vanilla materials in
`~/.cache/dh-tools/unpacked/`, dxc, wine+fxc_lite; see `shader-toolchain.md` section 9). Outputs (gitignored): `artifacts/shaders/B2/`, `artifacts/shaders/B3/`.

## B2: tinted RenderChunk (does the engine accept a modified vanilla material, and does our FS run?)
1. Quit the game. `cd <game>/data/renderer/materials`; `cp RenderChunk.material.bin RenderChunk.material.bin.orig`.
2. `cp <repo>/artifacts/shaders/B2/RenderChunk.material.bin .`; launch, load a world.
3. PASS: opaque terrain is red/pink, textures still visible (water, glass, leaves unaffected). FAIL variants to report: black/white/missing terrain
   (interface or sampler mismatch), crash on startup or vanilla look (file rejected: hash/integrity), or only some blocks tinted (variant coverage).
4. Restore: `cp RenderChunk.material.bin.orig RenderChunk.material.bin`.

## B3 result (2026-10-19): the loader does not go through ResourcePackManager

Three sessions of trace: no `materials: load renderer/materials/...` line ever appeared (neither `$load` overload), so
`materials.index.json` and the `.material.bin` files are read from `data/renderer/materials/` on disk by another path.
`DH_Terrain resolved: no` after every `loadList` is that, not a parse failure. Compiled names live in
`RenderMaterialGroup::switchable()` (1014 entries), not `common()` (137 legacy names).

Interim delivery: `scripts/deploy.sh client --game-materials` copies `DH_Terrain.material.bin` into the game's
`data/renderer/materials/` and adds the index entry (one-time backup `materials.index.json.dh-backup`);
`scripts/deploy.sh restore-materials` reverts both. A loader-level hook replaces this later.

## Trace run: which function loads `renderer/materials`?
Diagnostic hooks (log only, installed with the injector, so only when `materials/DH_Terrain.material.bin` is in the mod dir) on
`AppResourceLoader::load` (3 overloads) + `loadAllVersionsOf`, `ResourcePackManager::$loadAllVersionsOf` + `::load(loc, string&, optional<UUID>)`,
and `Core::StandardFileSystem::readFile / readFileData / openFile / readFileDataExt`. The `AppResourceLoader` and `StandardFileSystem`
ones are installed by vtable slot (their `$fn` thunks and `$vftable()` are not in the import lib; the vftable is resolved at runtime via the
mangled symbol). `FileSystemImpl::openFile` is skipped (not exported).
1. Launch the game, enter a world, then `grep 'materials: trace' <modDir>/logs/dh.log`.
2. `materials: trace hook <Name> -> N` lines at enable: `0` = installed; non-zero = the target did not resolve (vtable symbol lookup failed) and that hook is inactive.
3. `materials: trace[<Hook>] <path>` is logged once per distinct path containing "material" (any case), max 200 lines. Hooks that report a size print a
   following `trace   -> ...` line. The hook that shows `renderer/materials/materials.index.json` and `*.material.bin` is the loader entry point;
   the earliest/deepest one (e.g. `StandardFileSystem::$readFile`) can be used to splice the index and serve `DH_Terrain.material.bin`.
4. If nothing shows with "material" at all, the loader reads via a path the hooks do not cover (e.g. memory-mapped / `CreateFileW` directly).

## B3 (original plan): DH_Terrain loaded through the mod (no game-dir writes)
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

## Binding DH_Terrain through the mce -> dragon resolution

The engine picks a compiled material on every draw in `resolveDragonMaterial(Binding* sret, DragonCtx*, DrawState* ds, int pass)`. It reads the mce material via `*(RenderMaterialInfo**)(ds+0x148)`, derives a dragon name (`MeshFallback*`, `SingleBlock*`) plus variant flags, hashes them to `key[2]` and calls `cacheGetOrCreate(cache, Entry** out, key)`; if the entry has no material it calls `enqueueAsync(ctx, Holder* sret, Descriptor*)` (descriptor starts with `std::string name`, deep-copied), which later runs `CompiledMaterialManager::getMaterial(name)` -> CRT fopen -> `CreateFileW` (our redirect serves `DH_Terrain.material.bin`). `materials.index.json` gates nothing.

Our binding (`MaterialInjector.cpp`, installed behind `gActive`):
1. `ensureDhMceMaterial()` (render thread, from `spikes::dhMaterial()` once `fallbackMaterial()` is valid) copy-constructs a `mce::RenderMaterial` (0x378 B) from MeshFallbackPosUVNormalColor into the `switchable()` info named `DH_Terrain`; its info pointer is stored as `gDhInfo`. That makes `materialValid(dhMaterial())` true.
2. `resolveDragonMaterial` hook sets thread-local `tlDhResolving` when `ds+0x148 == gDhInfo`.
3. `cacheGetOrCreate` hook replaces `key[0]` with FNV-1a64("DH_Terrain") so DH gets its own cache entry (without it the DH binding would share the vanilla entry for that variant set).
4. `enqueueAsync` hook rewrites a `MeshFallback*` descriptor name to `DH_Terrain`.

Signatures (1.26.51, IDA-style; log line `materials: sig <name> -> 0x<rva>`; re-find on a game update from the function prologues):
- resolveDragonMaterial (RVA 0x08D3ABB0): `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 04 00 00 48 8D AC 24 80 00 00 00 44 0F 29 95 20 04 00 00`
- cacheGetOrCreate (0x052F94A0, checked unique at install): `55 41 57 41 56 41 54 56 57 53 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 4D 89 C6 48 89 D6 48 89 CF 49 8B 00 41 0F B6 48 08`
- enqueueAsync (0x0B786580): `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 C0 00 00 00 FE FF FF FF 49 89 D5`
- CompiledMaterialManager::getMaterial (0x0BCD6900): `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 A0 00 00 00 FE FF FF FF 4C 89 CB 4C 89 C6 49 89 D6 49 89 CF`
- `mce::RenderMaterial` copy ctor: SDK export `$ctor(RenderMaterial const&)` (RVA 0x05A5C0F0).

Evidence in the log after a run: `sig ...` lines, `DH_Terrain mce material cloned`, `dragon request MeshFallback* -> DH_Terrain`, `trace[CreateFileW] ...DH_Terrain.material.bin` + `redirect`, then `render: material DH_Terrain resolved`. Known risk: a parse failure gives an empty definition (undefined behaviour at bind).
