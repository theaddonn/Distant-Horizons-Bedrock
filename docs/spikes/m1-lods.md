# M1 checklist: LODs from the real pipeline (GLUE-4)

Log file: `~/.steam/steam/steamapps/common/MinecraftBedrockGDK/builds/Retail-1.26.5101-Levi/mods/DistantHorizons/logs/dh.log`.
Every run below ends with explicit PASS/FAIL lines; report each one.

Status: cross-compiled (client + server build, host tests green), **never run in game**. Everything below is a hypothesis
until you run it. Build + deploy: `scripts/build.sh client && scripts/deploy.sh client` (the block tables are read from
`<modDir>/data`). Logs: `logs/dh.log`. HUD: F7 toggles it, F8 dumps it to the log.

## Keys

| Key | Action |
|---|---|
| F5 | whole pipeline on/off (world driver + hooks + renderer). Turning it on switches far-plane mode to `local-proj-push`. |
| Shift+F5 | synthetic test feed — disabled unless the game runs with `DH_TEST_FEED=1` (it draws every uploaded section and turns the near cut-off off) |
| Ctrl+F5 | core RenderList frustum cull on/off (off = only the renderer's own 5-plane test; use if everything vanishes) |
| Shift+F7 | chunk ingestion on/off (default on) |
| F4 / Shift+F4 / Ctrl+F4 | primitive / far-plane mode / near cut-off (now on by default while the pipeline runs) |
| F6 / Shift+F6 / Ctrl+F6 | material / origin mode / transparent pass |
| Numpad 0 (or Ctrl+F8) | cycle the LOD draw hook: camera-after (default) / level-before / level-after / post-level (plain F8 = HUD dump) |

The old F7-ingest smoke test (`ChunkIngress`) is gone; ingestion is part of the driver.

## Run 1: new singleplayer world

1. Create a NEW world (default overworld, creative, any seed), load it, press **F5**.
2. Log must show, in this order (copy the whole block back):
   - `client driver: levels: ci->getLevel()=... getMultiPlayerLevel()=... getLevel()=... (isClientSide=?)` and possibly
     `getMinecraft(false)->getLevel()=...`  (this settles which accessor returns the server Level in SP)
   - `client driver: session started: SINGLEPLAYER (DhClientServerWorld)` (if it says REMOTE in SP, the server Level was not
     found: send the levels line)
   - `client driver: handles for dimension minecraft:overworld ...`
   - `store: 0 sections loaded (minecraft:overworld, ...)` BEFORE any `ingest`/chunk activity
   - `client driver: level [minecraft:overworld] loaded (dimension 0)`
3. Fly horizontally (creative flight) for ~300 blocks, preferably over land. Expected: coloured LOD terrain beyond the
   vanilla chunk edge (the near cut-off hides LODs inside the vanilla render distance). Take a screenshot.
4. HUD (F7) should show: `mode singleplayer`, `world.levels 1`, `tree.enabled >0`, `tree.loading`, `gen.queued`
   (drains over time as generation completes), `ingest.chunks` rising, `ingest.skipped`, `tick.ms` (single digits),
   `net.rx/net.tx` 0 in SP, `render.sections`, `render.listed`, `render.drawn`, `render.culled`, `render.cutoff`.
5. F8 and send the `hud-dump:` line.

PASS/FAIL (Run 1):
- P1 `session started: SINGLEPLAYER` appears within 3 s of F5 (FAIL: REMOTE or nothing; send the `levels:` line)
- P2 `store: 0 sections loaded` is logged before the first `ingest` activity
- P3 `ingest.drains` > 0 within 10 s (FAIL with `drain has been queued ... without running` in the log: the server-thread
  executor does not tick in this DLL)
- P4 `ingest.chunks` > 0 within 10 s, and `client ingest: rescan found N already-loaded chunks` with N > 0 (the chunks around
  spawn that were loaded before F5 are picked up by that rescan, radius 32 chunks)
- P5 `render.listed` > 0 within 30 s and `render.drawn` > 0 once you are past the vanilla edge. If listed stays 0 while
  `render.sections` > 0, the three tree counters say why — send them: `tree.enabled` (sections the tick enabled),
  `tree.culled` (core frustum rejects), `tree.notReady` (enabled but no GPU buffers yet). culled ≈ enabled → press
  Ctrl+F5 (core cull off) and report whether listed rises; notReady ≈ enabled → upload bookkeeping; enabled 0 → tick.
- P6 coloured LOD terrain visible beyond the vanilla edge (screenshot)
- P7 `gen.queued` falls over time (generation works); `world gen:` lines show no repeated errors

Expected around spawn / dimension change: the first 10-30 s only have LODs where chunks exist or generation finished
(nearest first). After a dimension change the new dimension's chunks noted during the switch are kept and ingested once its
level has loaded (the old dimension's pending chunks are dropped).

## Run 2: persistence

Quit to menu (log: `client driver: session stopped`, no crash), relaunch, load the same world, press F5.
Expected: `store: N sections loaded` with N > 0 BEFORE any chunk is ingested, and LODs appear almost immediately
(from LevelDB, before the tree needs to regenerate anything). Also check that `ingest.skipped` is large on the second visit
(`ingest.skipped` counts hash skips plus dropped chunks).

PASS/FAIL (Run 2): R1 `store: N sections loaded` with N > 0 before any `ingest` activity; R2 LODs visible within 10 s of F5
without flying; R3 `ingest.skipped` > 0 after the rescan.

## Run 3 (optional): dimension change and F5 off

Enter the Nether and come back: log `level of dimension N unloaded` then `level [...] loaded (dimension M)`; no stale
sections of the other dimension. F5 off must stop the session (`session stopped`) without a hang; F5 on restarts it.

## Run 4 (optional): remote server

Join a BDS with the server DLL: expect `session started: REMOTE`, `net: LevelInit for minecraft:overworld -> level key ...`
(if `sendLevelKeys` is on), `net.rx`/`net.tx` counting up, LODs from received sections. Joining a server WITHOUT the mod:
LODs only from chunks the client receives (sub-chunk ingestion). NOTE: the client sends `dh:frame` packets to a server
that may not know them; if joining a vanilla server gets you kicked, tell me.

## What to send back

- the `dh.log` block from F5 up to the first LODs (and everything from `client driver:`, `client ingest:`, `world gen:`,
  `render:` and `store:` lines)
- the F8 `hud-dump:` after flying
- screenshot(s); say which keys you changed (primitive / far mode / Ctrl+F5)
- crash log if any (see the risks below for where to look)

## Known risks / things only the game can answer

1. Which accessor returns the server Level in singleplayer (see the levels line). Wrong guess = REMOTE mode in SP.
2. `ClientLevelTickEvent` delivery: if absent the log says `ClientLevelTickEvent not seen, ticking from the render hook`.
3. Hook results (`hooking ... failed` lines) and the call frequency of `DBChunkStorage::saveLiveChunk` / `BlockChangedEvent`
   on the client DLL (block edits may not reach the LODs; chunk loads do).
4. `ll::thread::ServerThreadExecutor` in the client DLL (SP chunk drain and the generator hop through it). If chunks are
   noted (`ingest.chunks` stays 0) but never drained, this is the suspect.
5. Matrix layout for the core frustum: if the list is empty or tiny while sections exist, press Ctrl+F5 (core cull off).
   `render.listed` vs `render.sections` tells you.
6. World-gen quality: `Features` (default mode) now decorates the scratch chunks (trees, grass, flowers; engine feature
   placement via `decorateWorldGenPostProcess`, see PORTING.md; no structures). Expect trees on distant generated terrain
   and generation noticeably slower than Surface (9 base chunks per requested chunk, shared per 16-chunk batch; decoration
   is serialised per batch). Log: once `world gen: Features mode: ...`, first 10 tasks `... (base N ms), features=1 (M ms,
   ring chunks 9/9), nonAir=...`, and the 10 s line `(avg M ms incl. features, ...), features avg K ms`. `features=0` =
   decoration threw (base terrain delivered); a crash or junk blocks: set env `DH_BEDROCK_NO_FEATURES=1` to fall back to
   Surface. Trees may be clipped at chunk edges. UNVALIDATED in game. Scratch chunk generation is the
   least certain part of the port; `world gen:` log lines and `gen.queued` not draining mean it fails.
   Distant generation is OFF by default until validated (`common.worldGenerator.allowUnvalidatedGenerator=false` forces
   `generatorPlan=Disabled`, with a `config:` warning). To test, set it to `true`; look for the `world gen task N (x,z): ...`
   lines (first 10 tasks: ms, chunk state, nonAir count, top block at (8,8)). `FAILED after ... ms (null` = hop timeout or
   unsupported height. See PORTING.md "First in-game run".
   Parallel generation: on start `world gen: T scratch worker thread(s)`; every 10 s while active
   `world gen: N chunks in the last 10 s (avg M ms, in flight K, threads T)`. Expect N roughly T x 10 / (M/1000) (about
   30-50 chunks/s with 6+ threads vs ~9 before); `task` lines must still show nonAir ~30k and real top blocks. Bad terrain or
   crash only with T>1: set `common.worldGenerator.scratchThreads=-1` (legacy).
7. Exit ordering: the Dimension pointers die with the Level; the session is stopped from `ClientExitLevelEvent`. A crash on
   leaving the world is most likely the server Level being destroyed before the event.

## Decisions (GLUE-4)

- F5 = the whole pipeline (not just the renderer); Shift+F5 is the old test feed; Shift+F7 = ingestion on/off (on).
- The core render list culls with the DH far clip (`RenderUtil::farClipPlaneDistanceInBlocks`), not the vanilla far plane.
- worldMinY/worldMaxY: Java passes `minHeight` and `minHeight + level.getMaxHeight()` where Java's `getMaxHeight()` is the
  world HEIGHT. `ILevelInfo::maxHeight()` here is already the exclusive top Y (= min + height), so it is passed unchanged
  (no double add, no quirk reproduced).
- Near cut-off radius = vanilla render distance (chunks) * 16 * `RenderUtil::overdrawPrevention` (auto table + fast-movement
  reduction); the FOV-corrected near PLANE is a projection value and not used for the horizontal cut.
- Chunk updates carry independent snapshot copies of the 3x3 (lightChunk writes into neighbours); at most 12 updates are in
  flight per level.
- Store folders: singleplayer `worlds/sp_<world name>_<level id>/<dimension>/` (the level id keeps equal-named worlds apart),
  remote `worlds/remote/<levelKey or dimension>/` (two servers without level keys share a folder; add the server address
  when the packet layer exposes it).
- Already-loaded chunks (F5 pressed in a loaded world, Shift+F7 turned back on): there is no safe public enumeration of a
  ChunkSource, so a 65x65 chunk square around the player is probed with `getAvailableChunk` on the owner thread. Chunks
  farther than 32 chunks from the player at that moment are only picked up when they (re)load.
- `Options` changes are not pushed into a running world yet (`onOptionsChanged` is not wired); toggle F5 to apply.

## Run 5: draw hook

LODs drawn after the whole level (camera-after) show through Bedrock's depth-less translucent water. Stand at sea level
looking at the LOD edge and press Numpad 0 (log `render: draw hook -> <name>`, HUD `render.hook`). For each mode report:
(a) whether the under-water LOD faces disappear or tint blue, (b) whether LODs still appear at all. level-before is the
Java-like order (LODs before the translucent pass); level-after / post-level are for comparison.

## Run 6: near clip + fog

With the `DH_Terrain` material the renderer no longer drops sections inside the near cut-off radius; every listed section
is drawn and the fragment shader discards fragments whose horizontal camera distance is below the radius (Java DH
behaviour; the inner section's top faces still hide the border overdraw walls, vanilla terrain in front hides the rest).
The radius, fog start and fog end travel in the projective row of the world matrix (HUD `render.clip`, `render.fog`
"start..end"; fog start = `fogStartFraction` (0.6) x LOD far distance, end = LOD far distance). Ctrl+F4 toggles the clip
(off: radius 0, nothing discarded). Expect: no cliff ring at the vanilla edge, LODs fading to the vanilla fog colour toward
the LOD far distance. If LODs are invisible or misplaced with the custom VS, run `tools/shaders/build_dh_terrain.sh
--vanilla-vs`, deploy, and retest: if that draws, the custom VS matrix convention is wrong (report it). Other materials
still use the section cut-off (`render.cutoff`).

## Run 7: fade, shading, water

- **Dither fade**: `u_model[0]` row 3 entry 0 now packs `clipStart + fadeEnd * 4096` (both integer blocks <= 4095; `fadeEnd` = vanilla render distance in blocks). The VS decodes it (floor) and hands `light = (clipStart, fogStart, fogEnd, fadeEnd)` to the FS. FS: `d < clipStart` discards; in `[clipStart, fadeEnd)` an ordered 4x4 Bayer test on `SV_Position.xy` against `t = (d - clipStart) / (fadeEnd - clipStart)` discards, like the Java near-fade. `clipStart <= 0` skips both.
- **Face shading**: Java does NOT shade in the shader; `getShade` (down 0.5, up 1.0, north/south 0.8, east/west 0.6) is baked into the vertex colour on the CPU (`ColumnMeshBuilder` / `LodQuadBuilder` via `IClientLevel::shade`, already ported, `TableClientLevel` returns the vanilla constants). Applying it again in the shader would double-darken, so the shader factor table exists behind `DH_SHADE_IN_SHADER` (0). The DH direction byte is now carried to the shader anyway (`StageAVertex.u` = face index 0..5, `v` = 0; host test extended). If shading looks absent in game, check the baked colours first (`Level.cpp` `shadeByFace`).
- **Water**: `DH_TerrainTransparent` is a second cloned mce material (index entry `{"name":"DH_TerrainTransparent","path":"DH_Terrain"}`, same compiled shader): depth test on, depth write off, `Blending` (stateMask 1<<1), blend bytes copied from `entity_alphablend` / `beacon_beam_transparent` when valid (logged). The dragon rename hook treats both infos as DH. FS outputs the vertex alpha. The renderer draws `drawLayer(true)` with it and logs which material was used.

## Run 8: vanilla fog (disableVanillaFog)

Java DH (`MixinFogRenderer` / `MixinVanillaFogCommon.cancelFog`) overwrites vanilla fog start/end with huge values after
`setupFog`, unless the camera is in a fluid, has blindness, or DH is off. Bedrock port: hook
`LevelRendererCamera::$setupFog`, after `origin` write start = 0.9 x LOD far, end = LOD far (only with the pipeline on).
Which fields feed the shader is unknown, so Numpad 1 cycles the write set (HUD `render.vanillaFog`, log `vfog:` lines
with before/after values): `off`, `distance-fog` (mCurrentDistanceFog, mLastTargetDistanceFog, mBaseFog*, mLastFogEnd,
mBlendFogThisFrame=false), `fog-control` (mFogControl only), `all`. Default `all` (option
`client.graphics.fog.disableVanillaFog`, default true).

Underwater/lava/blindness: not detectable directly; heuristic skips the push when the current fog end is < 48 blocks.

In game: stand at the vanilla render-distance edge with LODs on, cycle Numpad 1; report which mode removes the grey band
on vanilla terrain, whether the log shows the fields changing, and whether underwater / lava / blindness fog still works.

## Run 9: lighting

DH_Terrain now samples the vanilla lightmap like Java DH (`uLightMap`): the FS gets a sampler `s_MatTexture` (slot 0, t0/s0, the
slot the MeshFallback table used) and the renderer binds `LevelRenderer::mBrightnessTex` (16x16, u = block light, v = sky light,
refreshed by the engine per frame for day/night and gamma) through the `renderMesh` texture-variant overload.
Light travels per vertex in `uv0.y = (sky*16 + block)/255` (`packLight`; the FS recovers the nibbles with `round(v*255)` and samples
texel centres `((block, sky) + 0.5)/16`). `texCoords` is `nointerpolation` in both VS variants and the FS (light is per quad).
`ConvertParams.cpuLighting` is set automatically: false for DH_Terrain (colour left unlit), true for the fallback materials
(old `lightFactor`, `skyBrightness`, v = 0). Config `lightmapSwapAxes` swaps the packed nibbles (= lookup axes) if the lightmap
orientation turns out to be reversed. `DhParams.w < 0.5` would disable lightmap sampling in the material (default 1).
Directional shade stays baked on the CPU; near clip, dither fade and far fog unchanged.

What to look for: night darkens the LODs like vanilla terrain (and follows gamma); caves, overhangs and tunnel mouths are dark; block
light from the chunk's own data (torches/lava/glowstone in generated LOD columns) brightens its surroundings with the usual warm
lightmap tint; no seam in brightness at the vanilla/LOD fade band. If everything is black or uniformly flat: the texture did not
bind (log "mBrightnessTex unavailable", or the UseTextures=Off material variant skips the sampler); if sky/block look swapped
(sunlit areas orange-tinted, torches blue-white): set `lightmapSwapAxes`.

(Superseded by Run 13 below: Numpad 3 now cycles the lighting modes, `mBrightnessTex` is no longer used, and the GPU
uv0.x is `(face + 1) / 8`.)
Run 9 follow-ups: Numpad 3 toggles `lightmapSwapAxes` live (HUD `render.lightmap` = `block,sky` / `sky,block` / `cpu`, `render.lighting` =
`lightmap` / `cpu`). If `mBrightnessTex` is missing, DH_Terrain is replaced by the MeshFallback/selection material with CPU lighting
(logged once). The FS reads the lightmap size with `GetDimensions` (texel centres for any size) and the real size is logged once.
## Run 10: clouds

Java DH (`CloudRenderHandler`): its own cloud texture, boxes drawn through the generic object renderer, vanilla clouds
disabled by a mixin while DH renders. Bedrock port (`CloudRenderer`, `CloudMesh`, `third_party/TinyPng.h`):

- **Vanilla clouds hidden** through `ViewRenderData::mDrawClouds`: the SDK has no cloud-draw function (only the
  `mCloudMeshes`/`cloudMaterial` fields), so the hook clears the flag on every non-shadow view before the vanilla world
  is drawn (`LevelRenderer::renderLevel` and `LevelRendererCamera::$render`, whichever sees the view first) and sets it
  back right after, so vanilla clouds return the frame DH stops (F5 off, Numpad 2 off, option off). Hidden only once
  the DH layer has actually been drawn (hand-off, no frame without clouds). Log lines `clouds: vanilla clouds hidden
  (true -> false) via ViewRenderData::mDrawClouds (level|camera hook)` / `restored`.
- **Which game facts the log prints once:** `LevelRendererCamera::mCloudHeight` (used as the DH layer height, fallback
  192), `mDrawClouds`, `mCameraAboveClouds`, and `CloudRenderObject` (`fancy`, `meshSize`, `gridSize`, `yTranslation`,
  colour). Read them to confirm the texel size / height assumptions below.
- **Pattern:** `<game dir>/data/resource_packs/vanilla/textures/environment/clouds.png` (read-only, found from the exe
  directory; 256x256 RGBA, white texels, alpha 255 = cloud / 1 = clear, 13356 of 65536 texels = 20%). Decoded by the
  vendored `TinyPng.h` (own inflate, MIT); a procedural blob pattern is the fallback (logged).
- **Geometry:** 12-block texels (Java), 4-block-thick boxes (top, bottom, walls; greedy-merged rectangles and wall
  runs, DH shading 1.0/0.7/0.9/0.8 baked into the vertex colour), alpha 0.8, white scaled by the sky colour. Covers the
  LOD far distance (square, + 384 block margin) around the camera, built on a worker thread in "cloud space" and
  uploaded through `MeshUploader` as a reserved section (detail level 0, never produced by the tree; skipped by the
  terrain draw). Rebuild only when the camera leaves the margin, the far distance / height / tint (10 s throttle) change,
  or the section vanished. Drift is the world-matrix translate: -X at 0.6 blocks/s (Java vanilla), wall-clock time.
- **Drawn** inside the LOD draw, after the transparent terrain layer, with `DH_TerrainTransparent`, near clip 0 and the
  DH far fog (start 0.6 x far, end far), so distant clouds fade into the fog colour. Only while the game itself would
  draw clouds (a non-shadow view had `mDrawClouds` set within the last second: respects the clouds video setting and
  dimensions without them).
- **Keys / config:** Numpad 2 toggles DH clouds (HUD `render.clouds`: on / building / waiting / off, `render.cloudQuads`);
  `client.graphics.clouds.renderDhClouds` (true) and `hideVanillaClouds` (true).
- **Known limits (shader is frozen):** tint is baked into the vertex colour (quantised, rebuild at most every 10 s), not
  multiplied in the shader; no alpha-sorted blending between cloud boxes; cloud shadows / weather darkening ignored.

In game (checklist):
1. F5 on, wait for LODs: log shows `clouds: pattern from ...clouds.png (256x256, 13356/65536 ...)` (or the fallback
   warning), the `mCloudHeight=..` line, `clouds: first update ...`, `clouds: first section live: N quads ...`,
   `clouds: vanilla clouds hidden ...`. Vanilla clouds vanish, DH clouds appear at the same height (192 expected) and
   reach to the horizon, fading into the fog.
2. Compare cell size with the vanilla clouds (toggle Numpad 2 off/on: the DH layer should line up with the vanilla
   pattern 1:1 if texel size 12 and the layer scrolls the same way; report mismatch size/speed/direction).
3. Fly up through the layer and walk under it: boxes visible from both sides, no hole below the camera, no flicker when
   the rebuild swaps in (walk ~300 blocks).
4. Day/night: clouds darken at dusk (tint steps, a short pop per step is expected); rain: vanilla greys its clouds, ours do not.
5. F5 off: vanilla clouds return immediately. Clouds setting off in video options / Nether / End: no DH clouds.

## Run 13 (lighting): checklist

Root cause of the pitch-black LODs: no uv0 vertex stream (see docs/PORTING.md "Run 13"). Expected after this build:

1. Load a world by day, F5 on (or startup with `client.enableRendering`). dh.log: `render: native vertex format ok:
   mFieldEnabled=110000100000000 (DH meshes also enable uv0=[4], uv2=[6]) ...` and, after the first upload,
   `render: DH mesh vertex fields uploaded: mFieldEnabled=1...1.1........` (position 4 must be 1). The 10 s line
   `render: lighting diag mode=shader lightmapOk=0 src=none flag=-1.00 skyDarken=1.00 ...` (flag = -skyDarken).
2. Default mode `shader` (HUD `render.lightmap=shader: none(shader)`): LODs lit like vanilla by day (sky-lit tops at
   full colour, not black); they should blend with vanilla terrain at the clip border.
3. Numpad 3 cycles: shader -> dynamic -> dynamic-swapped -> unlit-test -> light-debug -> shader (log line
   `render: lightmap mode -> <name> (Numpad 3)`).
   - `light-debug`: open terrain RED (sky 15), caves/overhangs darker, torches/lava add GREEN; blue 0.25 everywhere.
     Uniform dark blue = light 0 everywhere (CPU side; check `convertedV[...]` in the diag line). MAGENTA (R 1, B 1) =
     uv0 still not delivered (the VS fallback): report it, the vertex field index assumption is then wrong.
   - `unlit-test`: raw vertex colours (brighter than `shader` in shade, same by day on sky-lit surfaces).
   - `dynamic`: vanilla lightmap texture. Daylight brightness = the texture works (report it; it could become the
     default). Black = the entry is a placeholder; inverted (caves bright, open terrain dark) = try `dynamic-swapped`.
4. Night (`/time set night`): in `shader` mode LODs darken (blue-ish tint) and `skyDarken` in the diag line drops below 1
   (Java night value 0.2; Bedrock's night value is unverified, report it); torch-lit areas stay warm. Dusk transition is smooth (per-frame uniform, no re-upload).
5. Report: the mode colours above, the day/night look, and the dh.log lines from steps 1 and 3.
## Run 13: stars over LODs, cyan clouds

Root causes and RE evidence: `docs/PORTING.md`, "Run 13". Expected log lines after F5 (first frames):
`materials: DH clone forced transparent: stateMask=0x0002 ... blend=060803080f0100`,
`materials: DH clone forced late: stateMask=0x0002 ... blend=030203020f0100`, `materials: DH_TerrainLate mce material
cloned`, `render: sky order late -> opaque layer drawn with DH_TerrainLate`, `materials: DH binding first use:
DH_Terrain|DH_TerrainTransparent|DH_TerrainLate -> pass=N` (report the N per clone), `clouds: pattern from
...environment.brarchive:clouds.png (256x256, N/65536 cloud texels)`. HUD: `render.skyOrder`.

In game (checklist):
1. Night, clear sky, stand where LOD land and LOD ocean fill the horizon (sky order `late`, the default). Expect: no
   stars, sun or moon on LOD terrain or LOD water; stars only above the LOD skyline. Note whether far LOD sections now
   draw over near ones (that means the Transparent pass does not write depth).
2. Numpad 4 (or Ctrl+F7) -> `overdraw`: same expectation, at about twice the draw calls (`render.ms`). If `late` was
   broken (step 1) but `overdraw` is right, report it: `overdraw` becomes the default. `overdraw` repeats the draw
   at equal depth, so it needs depth func LessEqual: check that `materials: cracks_overlay_tile_entity depthFunc
   byte=6` was logged before the clone lines (otherwise the source's func is kept and the repeat may be rejected).
3. Numpad 4 -> `opaque`: the old behaviour (stars through the LODs) must come back. This confirms the pass-bin cause.
   If all three modes look the same, report the `pass=N` lines.
4. With `late`, Ctrl+F8 through the draw hooks: `level-before` is expected to show stars over the LODs again (it
   inserts before the sky); report the other hooks.
5. Sunset/sunrise: the sun/moon disc is hidden by LOD mountains in front of it.
6. Clouds from the ground: white/grey and translucent (sky visible through them at alpha 0.8), not cyan, not opaque.
   From above (fly to y > 200): LOD terrain visible through the layer, not brightened. Clouds fade out toward the
   horizon, and no square edge of the layer is visible. The cloud pattern now matches the vanilla clouds.png (compare with
   Numpad 2 off).
7. LOD water (sea beyond the vanilla distance): blue translucent with the seabed darkened, not brightened (it used
   the same broken blend). At the vanilla/LOD overlap band (about 115-144 blocks), check whether LOD seabed shows
   untinted through vanilla water (a known side effect of late/overdraw); compare with `opaque`.

## Run 13b: near-ring "LOD square", leftover stars, cloud tint

Deploy the DLL and `DH_Terrain.material.bin` together: the VS now decodes the cloud tint, and an old material would
discard the clouds. Expected log lines: as in Run 13; `clouds: pattern from ...environment.brarchive:clouds.png`.

1. White square over vanilla terrain (screenshot 28): go to the same spot at the same height (fly around y 190-200).
   Press Numpad 2 (DH clouds off). Expected: the square vanishes. It is a cloud cell of the y 192..196 layer just
   below the camera, and vanilla clouds return in its place. Then press Numpad 4 through late / overdraw / opaque with
   clouds on. Expected: the square does not change. If it survives Numpad 2 and changes with Numpad 4, report which
   mode removes it: that is the late-pass depth problem, and `overdraw` or `opaque` becomes the default.
2. Stand on the ground in the near ring (vanilla terrain within ~144 blocks) and look straight at nearby vanilla
   terrain with clouds off. Expected: no LOD surface on top of vanilla blocks, only the dithered fade at the vanilla
   edge.
3. Leftover stars at night, sky order `late`: (a) Numpad 2 off. If the stars disappear, they were showing through
   the translucent clouds (vanilla does the same). (b) Ctrl+F4 (near clip off). If stars vanish from the near ring,
   they came through the dither holes. (c) F5 off and the same view. If vanilla also shows stars on that terrain, it
   is vanilla behaviour. Report which of a/b/c removes them and where they were (near ring, LOD water, far LODs).
4. Clouds at dusk/dawn (or `/time set` 12000 -> 13000 -> 23000): the cloud colour follows the sky smoothly every
   frame, with no 10 s steps and no rebuild pops. The log shows no new `clouds: first section live` / rebuild
   lines while only the time changes. Clouds keep the same shape as before, with no holes around the player (the tint
   packing must not trigger the near clip).
5. Performance: report `render.ms` and `render.cloudQuads` with clouds on/off (the real pattern is about 825k quads).

## Run 14: LOD ice / water flickering black

Root cause and RE evidence: `docs/PORTING.md`, "Run 14". In short, the Transparent pass sorts draws back to front by an
unstable distance key. In `late`, a section's sea floor could therefore be drawn after its own ice and cover it.
The new default is `overdraw`, with the LOD water/ice drawn by the new depth-writing clone `DH_TerrainWater`. Deploy
the DLL and `DH_Terrain.material.bin` together, as before. The material file itself is unchanged, but the new index
entry `DH_TerrainWater` comes from the DLL.

Expected log lines after F5 (first frames):
- `materials: DH clone forced water: stateMask=0x0002 depthStencil=010006010103010101030101... blend=060803080f0100`
  (prefix as in the user's dh.log of build 1b56eee; what matters is byte 11, the 12th byte = `01`: depth write on,
  while the `transparent` line has `00` there).
- `materials: DH_TerrainWater mce material cloned (info=... material=...)`
- `render: sky order overdraw -> opaque layer drawn with DH_Terrain + DH_TerrainLate redraw; translucent layer with
  DH_TerrainWater (blend + depth write)`
- `materials: DH binding first use: DH_TerrainWater -> pass=N`. N should equal the N logged for `DH_TerrainTransparent`
  (and for `DH_TerrainLate`). `DH_Terrain` shows a different N, because it is the Opaque-pass copy; that is expected.
HUD: `render.skyOrder=overdraw`.

In game (checklist):
1. Daytime, frozen ocean (or any LOD ocean) filling the view beyond the vanilla distance, default sky order
   (`overdraw`). Walk, fly and turn for about 30 s. **PASS:** no black or dark section-sized patches on LOD ice or water,
   and the ice/water tint stays put while moving.
2. Numpad 4 (or Ctrl+F7) once: the order is late -> overdraw -> opaque, so from the default one press gives `opaque`.
   Log `render: sky order opaque -> ...; translucent layer with DH_TerrainTransparent (blend, no depth write)`.
   **PASS:** no black patches either. This is the reference: the floor is resolved before the water.
3. Numpad 4 again -> `late`. Log `... translucent layer with DH_TerrainTransparent (blend, no depth write)`.
   Move for at least 60 s (the order is not deterministic). **Expected:** the black flicker from the screenshot comes back. That confirms the Transparent-pass sort as the
   cause. If `late` does NOT flicker any more, report it: the cause is then something else.
4. Numpad 4 again -> back to `overdraw`. Night, clear sky, looking over LOD land and LOD ocean. **PASS/FAIL (important):**
   no stars, sun or moon on LOD land (as in Run 13 with `late`). If stars show on LOD land in `overdraw` but not in `late`,
   report it at once: the fix then needs a different approach. Report how visible stars are through LOD water or ice. Faint stars there
   are a known cost (PORTING Run 14, risk 2), so a screenshot is enough.
5. F7 HUD: note `render.ms` in `overdraw` vs `late` at the same spot (overdraw draws the opaque layer twice).
6. Flat LOD ocean seen at a grazing angle from high up (y > 150): check whether any far water section vanishes behind
   a nearer one (risk 3; it would look like a hole in the water showing the floor untinted). If step 1 still flickers,
   compare step 1 with step 2 closely: flicker in `overdraw` but not in `opaque` would mean the DH_TerrainWater depth
   write does not reach the GPU pipeline (risk 5).

What to send back: PASS/FAIL for steps 1-4 and 6, the `render.ms` pair from step 5, the four expected log lines above
(or their absence), the `pass=N` lines of all four clones, and a screenshot of any remaining patch, with the sky order
shown on the HUD.

## Run 13: LOD content (snow, ice, trees)

Background and root causes: PORTING.md "Run 13 (LOD content)". LODs already in the store keep their old content
(nothing is regenerated or wiped): look at a **new world** or terrain **never seen before** (fly far in one
direction). Distant generation needs `common.worldGenerator.allowUnvalidatedGenerator=true`, Features mode.

1. Log on the first world-gen work: `world gen: Features mode: base terrain of the batch + 2-chunk ring, vanilla
   decoration ...`, then (first 10 batches) `world gen features batch (x,z) 4x4: ... decorated 36 of 36 cells (0
   without base, 0 threw) ..., delivered 16, placedBlocks=N`. `threw > 0` or a crash: set env
   `DH_BEDROCK_NO_FEATURES=1` and send the log. The 10 s line's `placedBlocks .. (/chunk)` should be clearly higher in
   snowy biomes than before (one snow layer per column = ~256/chunk).
2. Frozen ocean / frozen river far away: white (snow on ice), with blue water only where vanilla leaves it open (the
   shore ring and unfrozen patches). Not lavender/blue-grey. Walking up to it: the real chunks look the same.
3. Snowy taiga / snowy plains far away: white ground, spruce trees; no blue-grey grass. Grass blocks elsewhere: green
   top, sides fading to dirt brown at the bottom (FADE_TO_DIRT, Java default).
4. Trees: no straight cuts along 16-block lines or 64-block section lines. Fly into an area that was only ever LOD:
   the real trees should pop in at the positions of the LOD trees (close match expected, not 100%: structures,
   vanilla-only order effects).
5. Measurement (optional, costs CPU): start the game with env `DH_BEDROCK_GEN_COMPARE=1`, fly into new terrain for
   ~1 minute. Each ingested real chunk is regenerated and logged: `gen compare (x,z): columns 256: topY same A, top
   block same B, tree columns real R gen G both X (IoU ..), snow top real .. gen .., ice top real .. gen ..,
   differing blocks D`, and every 10 chunks `gen compare TOTAL over N chunks (S skipped): ...`. Expect topY/top block
   same > ~230/256 and tree IoU > ~0.8 if the decoration reproduces vanilla; tree IoU near 0 means the seed is still
   wrong. The first compare of a chunk can show trees only on the gen side (a neighbour's spill not yet in the real
   chunk); the TOTAL line is the measure. Send the TOTAL lines.
6. Performance: generation is slower than before (64 base chunks per 16-chunk batch instead of 36, decoration serial
   per batch). Report the 10 s `world gen: N chunks in the last 10 s` line.

## Run 14: cloud cell "white box" near the camera

Background: PORTING.md "Run 14". Deploy the DLL and the rebuilt `DH_Terrain.material.bin` together (an old material
draws all cloud faces, as before). F5 on, DH clouds on (Numpad 2), video setting clouds on.

1. **From above** (fly to y ~200-230, look down at the cloud layer near the camera, ideally the same snowy peaks):
   each cloud cell shows its top and the walls that face you, translucent (terrain visible through the top at the
   same strength everywhere), not a flat opaque white block. Press **Numpad 6** (log `clouds: back-face cull -> off
   (Numpad 6)`): the cells become whiter and more solid (bottom and inner walls show through, the old look); Numpad 6
   again (`-> on`) brings the translucent look back. PASS: with the cull on, the box from screenshot 30 reads as a
   translucent cloud, not as terrain.
2. **From below** (y < 192, look up): bottoms and the outward walls, same translucency; Numpad 6 off/on makes the same
   difference.
3. **Inside the layer** (y 192..196): every face is drawn (the cull is off there by design); the clouds around you are
   visible, nothing disappears when you cross y 192 or 196 (one frame of change at the boundary is acceptable).
4. **Cloud probe:** press **Numpad 5** once with DH clouds on and once with DH clouds off (Numpad 2, vanilla clouds
   visible), each time from above the layer. Expected log: `cloud probe: requested (Numpad 5)`, then
   `cloud probe: vanilla drawClouds=.. aboveClouds=.. fancy=.. meshSize=.. gridSize=.. yTranslation=.. center=(..)
   origin=(..) color=(..) camera=(..)`, `cloud probe: vanilla meshes=N`, up to 4 lines `cloud probe: mesh i prim=..
   vertexCount=.. raw=.. positions=.. indices=.. colors=.. pos[min=.. max=..] aabb[..] ys={..} cols={..}` and
   `cloud probe: DH layer wanted=.. drawn=.. quads=.. slab y 192..196 (cell 12, alpha 0.80) backFaceCull=on
   (active if camera-relative=1)` (the default origin mode is camera-relative; in absolute mode, Shift+F6, the
   cull never runs). `ys` with two values 4 apart means vanilla boxes 4 thick, one value a flat sheet; `cols` gives the
   baked face shades and vertex alpha; `positions=0` means the CPU copy was freed after upload (then the AABB and
   vertex counts are the only geometry hint).
5. Far clouds unchanged: they fade into the sky at the DH fog end, tint follows day/night.

Send back: the `cloud probe:` lines (both runs), the Numpad 6 lines, and a screenshot from above with the cull on
and one with it off at the same spot; say whether the cells still look like solid boxes.

## Run 14: leave a world and re-join (crash in the generator, magenta LODs)

Background: PORTING.md "Run 14 (crash + magenta LODs after leaving and re-joining a world)".

1. Join a world, let LODs show with DH_Terrain (log `render: material DH_Terrain resolved`). Leave to the main menu,
   join the same or another world. **PASS:** LODs come back in normal colours within a few seconds; the log shows a
   new set of `materials: DH_Terrain mce material cloned` / `DH_TerrainTransparent` / `DH_TerrainLate` /
   `DH_TerrainWater` lines after the second join and no `invalid; drawing with selectionOpaqueMaterial` line.
2. Fly into new terrain in the second world for at least two minutes with Features generation running (10 s
   `world gen:` lines with `features avg`). **PASS:** no crash; the freeze/snow pass still produces white frozen
   oceans and snow cover on new LODs.
3. F5 off and on a few times in the same world: the LODs must keep DH_Terrain (no magenta) each time.

Send back: the second-join `mce material cloned` lines, any `invalid; drawing with` line, and whether a crash trace
appeared in `logs/crash/`.

## Run 15: render perf, late-repeat scope, overlap-band water, cloud tiles

Background: PORTING.md "Run 15 (render perf + residual visuals)". Deploy the DLL. The material file is unchanged, but
deploy `DH_Terrain.material.bin` with it as usual. Default sky order `overdraw`, late scope `beyond-vanilla`.

Keys (new: Numpad 7): Numpad 2 DH clouds on/off, Numpad 4 sky order (late -> overdraw -> opaque), Numpad 5 cloud
probe, Numpad 6 cloud back-face cull on/off, **Numpad 7 late-repeat scope `beyond-vanilla` <-> `full`** (log
`render: late repeat scope -> ... (Numpad 7)`, HUD `render.lateScope`), F5 pipeline on/off, Ctrl+F4 near clip.

Expected log lines:
- Every 10 s: `render: perf N fps | calls/frame opaque A late B water C clouds D | kquads/frame opaque .. late ..
  water .. clouds .. | ms/frame T (max M) opaque .. late .. water .. clouds .. | sections S, late skipped L, clip
  skipped K | cloud tiles X drawn / Y live, Z kquads live | resolves/frame dh .. trans .. late .. water .. | sky
  overdraw, late scope beyond-vanilla`.
- Once, a few seconds after the first frame: `clouds: first tiles live: ~232 tiles, ~670000 quads, built in ~100 ms (tile 1152
  blocks, radius 9216)`. There is no `clouds: first section live` any more.
- The log no longer gets ~17 `materials: DH binding pass=...` lines per second (only the first 4).
HUD: `render.cloudQuads` (live, ~670k), `render.cloudDrawn` (this frame), `render.lateScope`, `render.cutoff`.

In game:
1. **Perf A/B.** Daytime, standing on the ground with LOD land to the horizon, clouds on. Wait 20 s and copy two
   `render: perf` lines. **PASS:** `clouds` kquads/frame well under 200 (expected ~110 looking horizontally, a few
   looking straight down), cloud calls ~200-250 horizontally. Then Numpad 7 (`full`), wait 20 s, copy two lines:
   `late` calls go up by the `late skipped` count of the first lines. Numpad 7 back. Numpad 2 off: `clouds` drops to 0.
2. **Vanilla reference (needed for the proof).** Night, clear sky, F5 **off**. Look at vanilla terrain at the far edge
   of the vanilla render distance (hills 100-150 blocks away). **PASS:** no stars or moon in front of vanilla
   terrain. If stars do show on vanilla terrain there, report it at once: `beyond-vanilla` then leaves stars on
   LOD terrain in the 115..144 band, and `full` (Numpad 7) has to stay the default.
3. **Stars over LODs.** F5 on, same night view, default `beyond-vanilla`. **PASS:** no stars, sun or moon on LOD land,
   including the ring just beyond the vanilla terrain (the dither band at ~115-144 blocks). Compare with Numpad 7
   (`full`): it must look the same. Stars in the band only with `beyond-vanilla` = FAIL, send a screenshot.
4. **Overlap-band water (task 2).** Daytime, stand at a coast so that vanilla ocean reaches the vanilla render
   distance and LOD ocean continues beyond it, and look along the sea at the 115..144-block ring. **PASS** with
   `beyond-vanilla`: the vanilla water there keeps its blue tint over the seabed, with no untinted, sharper seabed
   patches in a ring. Numpad 7 (`full`): the untinted ring may come back (the old side effect). Screenshot both.
5. **Faint stars through LOD water/ice** (known, not fixed): night over LOD ocean. Report how visible they are; a
   screenshot is enough.
6. **Clouds.** Look around and fly 1000+ blocks in one direction at normal speed. **PASS:** no holes, square edges or
   missing tiles. Clouds fade at the horizon as before, and tiles appear ahead without a visible pop inside ~3000
   blocks. The vanilla clouds never flash on when you turn away from all DH clouds. From above and from below the
   layer, Numpad 6 gives the same difference as in Run 14. Inside the layer (y 192..196) every face shows.
7. **Near clip.** Walk around in the vanilla ring with LODs on: no LOD surface pops in or out at ~115 blocks
   compared with before (the skipped sections were fully discarded by the shader anyway). Ctrl+F4 (near clip off)
   still shows the LODs inside the ring.
8. **Cloud probe (task 4, still open):** with DH clouds **off** (Numpad 2, vanilla clouds visible), stand below the
   cloud layer and press Numpad 5 once, then fly above it (y > 200) and press it again. Expected: `cloud probe:
   vanilla meshes=N` and up to 4 `cloud probe: mesh i ... ys={..} cols={..}` lines. `ys` with two values 4 apart =
   4-thick boxes, one value = flat sheet. `cols` = baked face shades/alpha.

Send back: the four `render: perf` lines from step 1 (with the sky order / scope in each), PASS/FAIL for steps 2-4 and
6-7 with screenshots for 2-4, the `clouds: first tiles live` line, and all `cloud probe:` lines from step 8.
