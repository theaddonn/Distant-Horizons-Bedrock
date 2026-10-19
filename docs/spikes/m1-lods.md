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
