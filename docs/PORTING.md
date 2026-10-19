# Porting Distant Horizons to Bedrock — engineering log

This file is the running record of how the Java Distant Horizons (DH) core is
being ported to C++20 for Bedrock (LeviLamina 26.51.x). It holds the rules
every work package (WP) follows, the platform contracts that separate the
portable core from the Bedrock glue, and a dated list of decisions. Update the
**Decisions** section at every wave boundary; never rewrite history, append.

Java reference: `/home/lucy/Projects/Code/distant-horizons` (commit `2206fe7a4`),
core under `coreSubProjects/core/src/main/java/com/seibel/distanthorizons/core`.
Execution plan: `~/.claude/plans/plan-how-we-will-harmonic-hare.md`.

## 1. Layers

```
src/core/            dh_core  — platform-independent C++20 static library.
                     NO LeviLamina / Bedrock headers (mc/, ll/), NO exceptions,
                     -ffp-contract=off. Talks to the outside world only through
                     the interfaces in src/core/dh/platform/.
src/bedrock_common/  glue shared by client and server DLLs: implements the
                     platform interfaces over LeviLamina (BedrockChunk, BlockTable,
                     LevelDbLodStore, FileLogger, LeviExecutor, PacketTransport).
src/client/          DistantHorizons.dll (hooks, renderer backends, HUD, drivers).
src/server/          DistantHorizonsServer.dll (BDS: chunk hooks, world gen, net).
tests/               separate xmake project, Linux host, doctest, ASan/UBSan.
                     Only links dh_core; uses the test doubles in
                     dh/platform/testing, dh/thread, and the *Recording*/*Loopback*
                     classes next to each interface.
```

Rules of the seam:

* Anything that needs a Bedrock type lives outside `src/core`. If the core
  needs a new fact from the game, add it to an interface in `dh/platform/`
  (and to this file), do not include a game header.
* The core never allocates GPU resources, opens sockets, or reads game files;
  it hands bytes to `IVertexSink`, `ITransport`, `ILodStore`, `IFileSystem`.
* The core is driven by the glue: the glue calls into the core on its threads
  (`IExecutor` tasks) and the core calls back through listener interfaces.
* Every interface has an in-process test double so every algorithm can be
  exercised on the Linux host without the game.

## 2. Platform contracts (`src/core/dh/platform/`, WP0)

| Header | Replaces (Java) | One line |
|---|---|---|
| `BlockInfo.h` | `IBlockStateWrapper`, `IBiomeWrapper` | Plain structs; interned by the registry so pointer equality == state equality. `hashSerial()` FNV-1a replaces `Object.hashCode`. |
| `IBlockRegistry.h` | `IWrapperFactory.deserialize*`, `getAir/Water/PlainsWrapper` | `resolveBlock/resolveBiome(serial)` (nullptr if unknown), `air()`, `water()`, `defaultBiome()`, `*OrDefault` helpers. `MapBlockRegistry` for tests/tools. |
| `IChunk.h` | `IChunkWrapper` (subset the core uses) | relX/relZ 0..15, **absolute** y; heightmaps; non-empty bounds; DH sky/block light get/set; light-source list; `blockBiomeHashCode()`. `testing/TestChunk` reproduces `ChunkWrapper` heightmap rules. |
| `ILevelInfo.h` | `ILevelWrapper` + `IDimensionTypeWrapper`, `IClientLevelWrapper` | `minHeight/maxHeight/hasCeiling/hasSkyLight/isEnd/dimensionName/dhIdentifier/hashedSeed`; `IClientLevel` adds `blockColor(x,y,z,biome,block)->ARGB`, `dirtBlockColor()`, `shade(Direction)`. |
| `Direction.h` | `EDhDirection` | `enum class Direction {Down, Up, North, South, West, East}` (Java ordinals), `opposite`, `compassIndex`, `offsetX/Y/Z`, `kAllDirections`. WP-A aliases it under `dh/pos/`. |
| `IVertexSink.h` | `ColumnRenderBuffer` / GL upload | `MeshBuffers{opaque, transparent}` as vectors of byte buffers of 16-byte vertices, 64 bytes per quad, ≤2 MiB each; `upload(sectionPos, MeshBuffers&&, cb(bool))`, `remove(sectionPos)`. `RecordingVertexSink` double. |
| `ITransport.h` | plugin packet channel + `NetworkSession` identity | `SessionId=uint64`; `send(session, bytes)->bool`, `maxFrameBytes()`, `setListener(onConnected/onFrame/onDisconnected)`, `disconnect`. `LoopbackLink` pairs two in-process endpoints, delivers on `pump()`. |
| `IChunkGenerator.h` | `IChunkGenerator`, `EDhApiDistantGeneratorMode`, `EDhApiWorldGenerationStep` | `GenerationMode{PreExistingOnly=1,Surface=4,Features=5,InternalServer=6}`, `GenerationStep` int8 −1..9 (Terrain==LiquidCarvers==7), `queue(minCx,minCz,w,mode,step,onChunk,done)`, `isBusy()`, `cancelAll()`. |
| `ILodStore.h` | `FullDataSourceV2Repo` | `StoreRecord{pos,data,adjacent[4],genSteps,compressionModes,mapping,meta}`, `StorePart` bitmask, `StoreFlag{ApplyToParent,ApplyToChildren,Regenerate}`; `put/get(parts)/exists/erase/setFlag/positionsToPropagateUp|Down/positionsToRegen/regenCount/timestamp(s)/allPositions/flush`. Implementations in WP-D2. |
| `ILogger.h` | log4j `DhLogger` | `ILogger::log(Level, string_view)`, `dh::log::set/get`, `info/warn/...`, `logf` (snprintf). `NullLogger` default, `StderrLogger`. |
| `IClock.h` | `System.currentTimeMillis/nanoTime` | `nowMillis()`, `steadyNanos()`; `SystemClock`, `ManualClock`. |
| `IFileSystem.h` | `java.io.File` usage | `readFile`, `writeFileAtomic`, `exists`, `mkdirs`, `listDir` (sorted), `removeFile`; `StdFileSystem` (error_code overloads + C stdio, `_wfopen_s` on Windows). |
| `IExecutor.h` | `Executor` / `PriorityTaskPicker.Executor` | `execute(Task)->bool` (false = rejected after shutdown), `queueSize()`. `thread/ManualExecutor` (FIFO, `runOne/runPending/runAll`), `thread/InlineExecutor`. |
| `util/Result.h` | exceptions | `Result<T>` / `Status = Result<void>` with `Error{code, message}`; std::expected-subset API so it can be swapped for `expected-lite` later. |
| `util/Assert.h` | `LodUtil.assertTrue`, runtime exceptions | `DH_ASSERT(cond,msg)`, `DH_DEBUG_ASSERT` (only with `DH_DEBUG`), `DH_FAIL`; settable handler (`setAssertHandler`), default prints + aborts. |

`src/core/dh/platform/IncludeAll.cpp` includes every header above so both
toolchains compile them even before a consumer exists. Add new headers there.

## 3. Java → C++ semantic rules (apply in every WP)

* **No exceptions.** Return `Result`/`Status`; invariants via `DH_ASSERT`. Never
  `.at()`, `std::stoi`, `throw`. Decode paths return `ErrorCode::Corrupt`, they
  never crash on bad input.
* **Integer semantics.** Java `int` arithmetic wraps: use `uint32_t` where Java
  relies on overflow (hashes), `int32_t` elsewhere. `>>>` → shift on an unsigned
  type. Shift counts ≥ width are masked in Java; use WP-A `jshl/jshr`. `/` on
  negatives truncates in both languages, but `Math.floorDiv`/`floorMod` →
  `jfloordiv`/`& (n-1)`. `(int)double` saturates in Java → `satcast_i32`.
* **Floating point.** `float` where Java uses `float`, `double` where `double`;
  `-ffp-contract=off` on every target so host and MSVC agree bit-for-bit.
* **Ordering.** No dependence on hash-map iteration order; sort explicitly.
  `std::stable_sort` wherever Java sorts a `List` (TimSort is stable).
* **Signed bytes.** Java `byte` is signed: `GenerationStep` is `int8_t` and
  `DownSampled = -1` compares less than everything.
* **Coordinates.** Chunk-relative x/z are 0..15; **y is absolute** everywhere a
  Java method took `relY` (it was absolute there too). `minHeight` inclusive,
  `maxHeight` exclusive.
* **Quirks are kept** when they affect persisted or rendered output (they are
  listed per WP in the plan's "Must-keep invariants" column). Document each
  reproduced quirk in a comment next to it with the Java file:line.
* **Hashes are not Java-compatible** where Java hashed object identity
  (`IBlockStateWrapper.hashCode`); ours are deterministic across runs/platforms
  (`hashSerial`), which is what the persisted chunk-hash skip needs.
* **Threading.** Core code receives an `IExecutor` and an `IClock`; it never
  creates threads itself except in WP-H's pool implementation. Everything must
  be testable with `ManualExecutor` + `ManualClock`.

## 4. Decisions

Format: date — WP — decision — why — evidence.

### 2026-10-18 — WP0 — platform contracts frozen

* **Own `Result`/`Status` instead of vendoring `expected-lite` now.** Third-party
  vendoring is a separate WP; the API is a strict subset of `std::expected`
  (`has_value/operator bool/value/operator*/->/error/value_or`) so the swap is a
  type alias later. `value()` on an error is a `DH_ASSERT`, not UB.
* **Assertion handler is process-global and replaceable.** Tests install a
  recording handler; production aborts. `DH_DEBUG_ASSERT` compiles out unless
  `DH_DEBUG` (set by both xmake projects in debug mode).
* **Interning by serial string.** `BlockInfo`/`BiomeInfo` are plain structs
  owned by the registry; the core only ever holds `const BlockInfo*`. Unknown
  serials resolve to `nullptr` (not air) so decoders can warn; the
  `*OrDefault` helpers give Java's fallback where wanted. `water()` added
  because `FullDataToRenderDataTransformer` needs the water state.
* **`IChunk` keeps Java's lazy heightmap semantics but drops
  `createDhHeightMaps()`.** Implementations compute on first use. Two Java
  quirks are reproduced and tested in `TestChunk`: the block at
  `minNonEmptyHeight` is never examined by the heightmap scan, and
  `maxNonEmptyHeight()` of an all-empty chunk is `inclusiveMin + 16` (the Java
  doc comment says `exclusiveMax`; the code does not).
* **`IChunk::blockBiomeHashCode` ported verbatim incl. the `lightBlockingY`
  (sic) sampling in the solid branch**, with `hashSerial` for the wrapper
  hashes. Not Java-equal (impossible), but deterministic.
* **`Direction` lives in `platform/Direction.h`** for now because WP0 must not
  create `dh/pos/`; WP-A aliases/moves it. Ordinals = Java `EDhDirection`.
* **`IVertexSink` fixes the 16-byte vertex layout and the 64-byte quad
  granularity** at the contract level (see header). Stage A converts to
  Bedrock's 24-byte `MeshData` in the glue; the core never sees `mce::`.
* **`ITransport` is frame-oriented, not stream-oriented.** One frame in, one
  frame out, ordered per session; `maxFrameBytes()` constant so WP-K2 splits
  payloads deterministically. `LoopbackLink::pump()` delivers only what was
  queued at call time so tick-based tests (bandwidth) are exact.
* **`ILodStore` flags are a bitmask, not Java's nullable `Boolean`.** The
  store treats "unset" as false (Java queries did too); tri-state stays an
  in-memory concern of the `FullDataSource` merge (WP-B2). Adjacent strips are
  indexed by `compassIndex(Direction)` (N=0,S=1,W=2,E=3).
* **`GenerationMode` values are the Java `complexity` bytes** (1,4,5,6), not
  ordinals, so comparisons mean "more complete terrain".
* **Windows CRT:** `_wfopen_s` instead of `_wfopen` (deprecated under `/W4`).
  Noted but not changed (INFRA-build's file): clang-cl ignores
  `-ffp-contract=off` with a warning; the MSVC-style spelling is `/fp:precise`
  which is already passed, so fp contraction is already off there — the extra
  flag can be dropped from `src/core/xmake.lua` for the windows branch.
* **Builds must pass `-P .`** in both the repo root and `tests/`: without it
  xmake resolved the project to the main checkout from inside a worktree.

## Follow-ups recorded during Wave 1 reviews

- **COMPARE_SHIFT lesson (2026-10-18):** `RenderDataPointUtil.COMPARE_SHIFT` is `0b111` = 7. The exploration notes, the WP-E0 port, its reviewer and the dumper README all carried `>>3`; only the Java-generated `rdp` golden suite caught it. Rule: constants are read from the Java source, never from a summary; every WP gets a golden replay, not just a stub.
- **WP-H → WP-G2:** Java `LodRenderSection.java:490` calls `renderLoaderExecutor.remove(runnable)`. `dh::Executor` has no `remove` (std::function is not comparable). G2 must use a cancellation token/`shared_ptr<atomic<bool>>` captured by the task, or `Executor::execute` must return a handle.
- **WP-H `DelayedSaveCache`:** Java slept 100 ms before running removal handlers to cover callers that mutate a container after `writeToMemoryAndQueueSave` returns. The port has no such window; concrete caches (data source / beacon) must mutate inside the first lock or via a `modify` callback. `KeyedLock` is non-reentrant (Java `ReentrantLock`): never lock the same key twice on one thread; removal handlers run with the key lock held.
- **WP-H:** thread count / run-time ratio are fixed at construction; a config change must rebuild `ThreadPools` (Java rebuilt pools in `onConfigValueSet`). `threadPriority` is not applied to workers.
- **WP-D2:** `ILodStore::timestamps` end bound is EXCLUSIVE (Java `BETWEEN … end-1`); `regenRecordCount` is a record count, not Java's 2^(detail+6)-weighted `getRegenChunkCount` — callers derive weights from `positionsToRegen`.
- **WP-C2 (verified Java quirk):** `lightChunk` clears the center chunk after seeding, so in the center a torch cell reads 13 and open-sky cells read 14 while neighbour chunks read 15. Reproduced on purpose; the Bedrock glue should supply native light where available.
- **WP-E2 (pending):** Bedrock snow layers are `minecraft:snow_layer[height=N]`; the Java rule looks for `layers=N` on `minecraft:snow`. The glue's serial format or the transformer's matcher must bridge this.
- **WP-G1 → WP-G2:** `QuadTree::mutateOutOfBoundChildNodes` skips a child whose value is null and never descends into it (Java behaviour); a chain with a null value in the middle is not mutated below the first null. `NodeIterator::remove()` keeps the iterator valid (descendants purged from the BFS queues before deletion). `MovableGridRingList` has no lock (Java `ReentrantReadWriteLock`) — G2 decides locking.
- **WP-K1 deliberate deviation (user decision 2026-10-18):** `LevelInitMessage::decode` validates level/server keys and the 150-char limits up front; Java accepts any bytes at decode and validates later in `ClientPluginChannelApi`. Consequence: Java-valid frames with unusual key characters are dropped at our frame layer. The `net` golden replay counts Java's random-key `level_init_N` frames as skipped for this reason.
- **WP-J2 → platform drivers (SingleplayerGenDriver / BdsWorldGenerator):** `IChunkGenerator::queue` delivers `std::shared_ptr<IChunk>` per chunk (Java kept the wrappers until the batch ended; lighting needs the 3x3). The driver hands over the scratch-chunk wrapper it allocates and must not reuse it. Drivers are expected to light BLOCK light (`lightChunk`, `generateSkyLight=false`) on chunks with `!isDhBlockLightingCorrect()` before `onChunk` (Java `DhChunkGenerator:735-775`); `WorldGenerator` only has a block-light fallback. Sky light is baked per section by the updater at detail 6. Destruction order: LodRequestModule -> retrieval queue -> GeneratedProvider.

## Follow-ups recorded during Wave 2/3 reviews and golden replays

- **E2 air handling (golden-caught):** Java's ignore/cave sets decide by SET MEMBERSHIP (`FullDataToRenderDataTransformer.java:469-470`) and the sets are pre-seeded with the single canonical `AIR` wrapper (`BlockStateWrapper.java:1156,1169`); a *distinct* air state (e.g. `cave_air`) is NOT ignored and goes through the normal non-solid paths. An earlier "fix" that tested `isAir` was reverted after the `f2r` replay disagreed in 138/608 cases. Only the carry-over reset uses `isAir()` (`:428`). On Bedrock there is only `minecraft:air`, so both coincide in practice; the Java semantics are kept for the oracle.
- **D3 listeners:** `SourceUpdater` fires update listeners AFTER releasing the position `KeyedLock` (Java fired under a reentrant lock). `updateLocked()` callers must call `notifyListeners(pos)` after unlocking. `removeListener` cannot stop an in-flight notification — use `inFlightUpdates()`/`waitQuiescent()` before destroying a listener's target; G2 registers one tree-level listener and looks sections up by pos.
- **D3 propagate order:** Java's propagator iterates parents in `HashMap` order and SQLite's `ORDER BY Distance LIMIT n` leaves ties unspecified, so `updateOrder`/pass counts are not asserted by the `propagate` replay; only the final stored sections are (they match in all 33 Java scenarios). The replay needs `worldCompression = VisuallyEqual` (the dump used it) and gates 15 slow cases behind `DH_GOLDEN_SLOW=1`.
- **J2 chunk ownership:** `IChunkGenerator::ChunkCallback` delivers `std::shared_ptr<IChunk>`; the generator must not reuse a delivered chunk. `WorldGenerator` keeps the batch alive until processing finishes (Java kept the wrappers in `chunkList`). Platform drivers (SingleplayerGenDriver, BdsWorldGenerator) light BLOCK light before `onChunk` (Java `DhChunkGenerator.java:735-775`); sky light is baked per section by the updater at detail 6. `setCanRegenerate` follows `LodQuadTree.java:948-959/:985`, not an unconditional `false`.
- **Key bindings (client):** F4 primitive/winding, Shift+F4 far-plane mode, Ctrl+F4 near cut-off, F5 whole pipeline (world driver + renderer), Shift+F5 test feed, Ctrl+F5 core frustum cull, F6 material, Shift+F6 origin mode, Ctrl+F6 transparent pass, F7 HUD, Shift+F7 chunk ingestion (default on with the pipeline), F8 HUD dump, Ctrl+F8 LOD draw-hook point (camera-after / level-before / level-after / post-level), F9 R0 inventory, F10 / Ctrl+F10 / Shift+F10 / F12 / Shift+F12 render spikes (F11 = Bedrock fullscreen, unused). See `docs/spikes/README.md`.

## GLUE-3 review follow-ups (server driver)

- **Level naming rule (shared by the server driver and GLUE-4), `src/bedrock_common/LevelNaming.h`:** `levelKey = sanitise(levelKeyPrefix + worldName + "_" + dimensionResourceLocation)` (`[a-zA-Z0-9-_]`, <= 150); `levelName = sendLevelKeys ? levelKey : dimensionResourceLocation`. `FullDataSourceRequest.levelName` is compared byte-for-byte by `ServerNetwork::findLevelByName`. The client sends `RequestLevelInit(dimensionResourceLocation)`, takes the key from `LevelInit` and uses it as `levelName`; if the server has `sendLevelKeys` off there is no `LevelInit` and the client uses the dimension resource location (e.g. `minecraft:overworld`). Changing `sendLevelKeys` needs a server restart (LevelInfo is built once per dimension).
- **Settings threading:** `ServerPlayers::settings` is written on the transport dispatch thread (`PacketTransport::runOnDispatch`), the same thread `ServerNetwork::onFrame` reads it on. Core readers on worker threads (request handlers) remain unsynchronised; the glue's own world-gen gate uses a separate atomic.
- **G2 (review-caught, HIGH):** an EMPTY full-data section still produces an empty-but-renderable render section in Java (`FullDataSourceProviderV2.get :210-213` → `createEmpty`; `transformCompleteFullDataToColumnData :123-127`; `LodRenderSection.java:191-195` uploads an empty container → `canRender()` true). Returning "nothing" instead made world-gen unreachable (only `enableDeleteChildren` nodes request generation) and re-transformed empties every tick. Rule: empty sections upload once with empty `MeshBuffers`.
- **G2 upload identity:** `IVertexSink::uploadWithId/removeIf` exist so a disposed section's late callback cannot wipe the replacement section's buffers at the same pos (Java closed its own container object). Sinks must implement real ids; defaults only keep old sinks compiling.
- **G2 lifetimes:** retrieval callbacks capture a `shared_ptr<RetrievalState>`, sections a `shared_ptr<const TreeShared>` — never `this`/`LodQuadTree&`. `RenderList` reads the tree centre through an atomic. Deterministic near-to-far render order replaces Java's effective hash order (`SortedArraySet.add` never sorted).
- **Build gotcha (2026-10-19):** once `dh_core` had ~50 objects xmake switched the archiver call to an `@response` file; the `link.exe` wrapper's librarian filter only strips `/opt:*` from argv, so llvm-lib saw `/opt:ref` inside the response file. Fixed project-side with `set_strip("none")` on the static target (strip flags are linker-only).

## First in-game run (2026-10-19, user-run; nothing launched by agents)

- **Works:** HUD + F8 dump; SP detection (`mode singleplayer`); `ClientLevelTickEvent`; `ll::thread::ServerThreadExecutor` ticks in the client DLL (1703 drains); chunk ingestion (957 chunks, hash skips counted); 174 sections uploaded through the `MeshUploader` (214 MB); R1 (F10) draws a world-space quad.
- **R3 decided:** `zfar-override` (enlarging `mZFar` in `setupCamera`) makes the vanilla clouds disappear; `local-proj-push` is fine → it is the default `FarMode` now. Culling uses the DH far clip inside the pushed projection.
- **Bug (fixed):** default `generatorPlan = SurfaceThenChunks` needs an `IRoughGenerator`; Bedrock has none, so every coarse retrieval failed with `No rough generator provided` (≈60k errors in a minute, rotating the log away). `ConfigBridge::init` now clamps surface plans to `ChunksOnly` like Java's `WorldGenPlanConfigEventHandler`, and the two gen-failure log sites are throttled (first 20, then every 1000th).
- **Open:** `render.listed` stayed 0 with 174 live sections. The HUD could not tell whether the tree enabled nothing, the core frustum culled everything, or the enabled sections had no GPU buffers → new counters `tree.enabled` (tick-enabled), `tree.culled`, `tree.notReady` (`ClientLevelModule::lastRenderListStats`). Next run decides: culled ≈ enabled → matrix layout (Ctrl+F5 to confirm); notReady ≈ enabled → upload callback / identity; enabled 0 → tick/expected-detail.
- **Test feed:** Shift+F5 needed a drawn frame first and its sections were hidden while the pipeline's list was active; the render hook now records the camera even when the renderer is off, and an active feed draws every uploaded section (`drawAllUploaded`).
- **Keys:** F11 is Bedrock's fullscreen toggle → R1b moved to Ctrl+F10, spike primitive to Shift+F12.
- **Second run (same day):** with Ctrl+F5 (core cull off) `render.listed` went 0 → 417 and 118 sections drew — LODs render through the real pipeline. So the core `Frustum` was fed the wrong matrix: `tree.culled ≈ tree.enabled`. The renderer's own `Frustum5` (planes from `transpose(view * proj)` for Bedrock's row-vector matrices, AABB in camera-relative coords) was right, so `RenderListParams` gained `worldViewProjection` (column-vector world VP, `transpose(view*proj) * T(-cam)`), and the core uses it instead of re-deriving Java's `setTransposed(mcModelView).translate(-cam)` composition. `proj=unknown, right-handed` in the first-draw line means `mZNear/mZFar` do not describe the matrix (infinite/reversed Z suspected) — `patchFar` still produced a usable far plane for the R3 quad.
- **Material:** `selectionOpaqueMaterial` drops the green channel (trees black, sand pink) → `MeshFallbackPosUVNormalColor` is the Stage-A default. Its `MaterialPtr` must be re-resolved when invalid: the group's entries are replaced on clean reloads (world exit), which left the static pointer dangling (`R1b … material invalid`). `DH_Terrain` is spliced into `materials.index.json` but never resolves after `loadList(materials/common.json)` — the vanilla pack's `common.json` lists legacy `*.material` files, not index names, so Stage B still needs the real load path (open).
- **Third run — R0 answers:** `RenderMaterialGroup::common()` holds 137 legacy-named materials (`ui_*`, `particles_*`, `selection_overlay*`); the compiled `materials.index.json` entries (1014, incl. `MeshFallbackPosUVNormalColor`, `RenderChunk`, our spliced `DH_Terrain`) live in **`switchable()`**. `MaterialPtr{common(), "MeshFallbackPosUVNormalColor"}` therefore never resolved — look compiled names up in `switchable()`. `getMaterialInfo()` creates an entry on demand (`info=1 loaded=0` for everything), so "info exists" proves nothing; only `mPtr` does. D3D12 is active (`d3d12=1 d3d11=0`), viewport depth 0..1. The projection is column-vector `[.. | 0 0 -1.00002 -1 | 0 0 -0.05 0]` = ZO 0..1 right-handed, near 0.05, far 2500 — but `cam.mZNear/mZFar/fov/aspect` all read 0 (wrong field or unused), which made `analyzeProjection` report `unknown` and `patchFar` divide by zero; near/far are now derived from the matrix. The tessellator probe's seed colour is magenta (`0xffff00ff`) and RenderDragon's unset-uniform default is magenta too, which explains the selection material's tint (its colour uniform is never set by us).
- **Fourth run:** M1 reached — LODs from the real pipeline, correct colours, core frustum culling right (`tree.culled 265/423`), near cut-off active, `MeshFallbackPosUVNormalColor` resolves from `switchable()`. Vanilla fog whites everything out past the vanilla render distance (as expected for a vanilla material) — Java solves this with its own shader. `DH_Terrain` still unresolved: the `ResourcePackManager::$load` hook never saw a `renderer/materials` path in any session, so the compiled loader reads `data/renderer/materials/` from disk directly. Diagnostic only (not a shipping path — the mod must stay inside its mods dir): `deploy.sh client --game-materials` patched the on-disk index + file once; after a restart `switchable().getMaterialInfo("DH_Terrain")->mPtr` was STILL null, so either the loader rejects our `.material.bin` or materials are not loaded by index name on demand. Reverted with `restore-materials`. Projection detection from the matrix now reports `ZO 0..1, right-handed, near=0.050 far=2512`. Next: find the real loader / an in-process registration (agent research in progress).
- **First LODs with our shader (fifth run):** `DH_Terrain` bound through the mce→dragon resolution (signature hooks + cloned mce material) and the engine's own loader parsed our `.material.bin`; LODs render in true colours with no fog once our FS is in *every* MeshFallback pass (the rasterized path uses `RasterizedOpaque`, not `Opaque`). Placement is correct (probe: section world y −64..70 at sea level 62, min corner = camera section), so the "city skyline" look — vertical column walls from y=−64 to the surface along every section border, rows stacked behind each other — is a mesh-adjacency problem (border faces not culled against the neighbouring section), not a transform problem. Also: the Shift+F5 test feed left on disables the near cut-off (`render.cutoff 0`), which is why LODs overlapped vanilla terrain in several runs.
- **Correct LODs (sixth run, 2026-10-19 13:00):** two more root causes behind "walls"/"dome"/"far draws over near": (1) the `Rasterized*` passes of MeshFallback use a 384-byte vertex cbuffer (`FogControl@0 u_viewProj@16 u_model@80`) while `Opaque/AlphaTest/Transparent` use the 464-byte one (`u_proj@0 u_view@64 u_model@144`) — a VS must be built per layout (`dh_terrain_raster.vert.hlsl`, `build_material.py --pass-code/--pass-uniforms/--pass-cbuf`); (2) the mce material cloned for DH inherited the clone source's render state, and the only source valid at first draw was `selectionOpaqueMaterial`: stateMask 0x0172 (Blending|DisableAlphaWrite|DisableDepthWrite|StencilWrite|EnableStencilTest), depthWriteMask 0, depth bias −100/−15 — no depth writes between our own sections. `ensureDhMceMaterial` now forces stateMask 0, depth test+write, no blend, bias 0, depthFunc byte copied from a JSON-`LessEqual` material (`cracks_overlay_tile_entity`, byte 6). Enum values for `mce::ComparisonFunc`/`DepthWriteMask` are not in the SDK; D3D numbering (write mask 1 = All) worked. Result: Java-identical terrain, 59 fps at 46 sections drawn, 0.9 ms render.
- **Perf note:** 118 sections drawn cost ~27 ms (`render.ms`) at 660 KB average per section (24-byte vertices); the immediate `renderMesh` path is not viable long-term — B4 (16-byte DH vertex) and fewer draw calls are the next render work.
- **Scratch chunk generation (sixth run, unvalidated fix):** `dimension.mWorldGenerator->createNewChunk` returned null by construction (`WorldGenerator` does not override `ChunkSource::createNewChunk`; only `MainChunkSource`, `WorldLimitChunkSource`, `ChunkViewSource`, `NetworkChunkSource` do, and the base forwards to `mParent`, which a generator lacks), so the `LevelChunk` constructor fallback ran on a pool worker, where `WorldGenerator`'s thread-local state (`mThreadLocalHeightmapProvider`, `generatorHelpersPool`) is not set up; the "city of random columns" beyond the real chunks is the likely result. `BdsWorldGenerator::generateScratch` now skips `createNewChunk`, and runs ctor + `tryChangeState` + `loadChunk` + snapshot on the server thread (15 s TTL), rejecting chunks with zero non-air blocks, and logs the first 10 tasks (`world gen task N (x,z): ... state a->b, nonAir=..., top(8,8)=...`). **Safety:** `ConfigBridge` forces `generatorPlan = Disabled` unless `common.worldGenerator.allowUnvalidatedGenerator` (default false) is true in config.json; a warning is logged. Set it true for the next in-game check; once the task lines show plausible terrain (nonAir ~ 30-60k, top block grass_block/stone/water at a sensible y) the default can flip.
- DH_Terrain is bound in-process: the mce material is cloned from MeshFallbackPosUVNormalColor and the engine's per-draw dragon resolution is redirected to our compiled material via signature-resolved hooks (`MaterialInjector.cpp`; mechanism and signatures in `docs/spikes/stage-b-offline.md`). Check the `materials: sig ...` log lines first after a game update.

## WP-L (world/level lifecycle) review follow-ups

- **Explicit update rejection:** `SourceUpdater::updateAsync` completes with `Error(ErrorCode::Rejected)` when the updater is shut down or the executor refuses the task; `Level::finishSave` commits the chunk hash only on `has_value() && !provider_->isClosed()`. `Ok(false)` (section unchanged) still commits the hash — this matches Java, where `onDataSourceSaveAsync` saves the hashes after `updateDataSourcesAsync` completes regardless of whether anything changed (`AbstractDhLevel.java:177-221`).
- **Level close is self-sufficient:** `AbstractLevel::close()` gates new chunk tasks, then waits (bounded by `closeWaitMs`, 5 s per wait) for in-flight chunk tasks, outstanding saves, the updater's pending/in-flight updates and the propagator. On timeout it logs an error and deliberately leaks the storage objects instead of freeing memory a straggler may still touch. With a busy shared pool and no `drainExecutors` hook, `close()` can therefore block its caller for up to ~10 s.
- **Network listener barrier:** `DhClientLevel::close()` resets `alive_`, detaches the handlers and waits for the `weak_ptr` to expire, so no listener body outlives the level. Calling `close()` from inside a listener would wait on itself (5 s, then an error) — never do that.
- **`WorldTicker::poll`** advances `last_ += period` (catch-up) and resyncs to `now` only when more than two periods behind, so jitter does not drop ticks; after a 1–2 period stall two ticks may fire back to back.
- **Deliberate Java deviation — SP server unload:** `DhClientServerWorld::unloadLevel` calls `onUnload()` on the server handle AND every attached client wrapper; Java (`DhClientServerWorld.java:157-163`) only calls it on the server wrapper. Bedrock's `closeImpl` does the same, and the glue's `ILevelHandle::onUnload` is idempotent, so the extra call is harmless and keeps client-side cleanup from depending on who unloads first. `DhApiLevelUnloadEvent` has no Bedrock equivalent yet.
- **Renderer stop vs. render list:** `ClientLevelModule::stopRenderer()` closes the tree under `renderMutex_` so a `collectRenderList` build in flight finishes before the sections are disposed (lock order `renderMutex_` → `mutex_` → `treeTickMutex_`, same as `collectRenderList` → `tree()`).

## GLUE-4 (client world driver) notes

- `src/client/world/`: `ClientWorldDriver` (lifecycle, WorldDeps, worker tick, render list), `ClientIngest.cpp` (hooks, pending set, snapshot drain), `ClientLevelHandle`. Singleplayer = `DhClientServerWorld` over the in-process server Level (chunks + `BdsWorldGenerator` from the SERVER dimension; the client dimension renders); otherwise `DhClientWorld` over `PacketTransport`. Checklist: `docs/spikes/m1-lods.md`.
- `BdsWorldGenerator` and `KvChunkHashStore` moved to `src/bedrock_common/` (shared by server and client).
- Per-level close must not shut the pools down (dimension change): the platform `drainExecutors` waits (bounded 5 s) for idle pool executors; the pools are shut down and joined only in `stopSession`.
- Additive core accessor: `AbstractLevel::inFlightChunkUpdates()` (ingestion back-pressure).
- Chunk updates get independent snapshot copies of the 3x3 neighbourhood: `lightChunk` writes seeds into every chunk of the 3x3, so a snapshot shared between two queued updates would race.
- Thread map: game thread (tick event, handles, remote ingestion), worker "DH Client Driver" (level load/unload, world tick), server thread (SP ingestion drain), render thread (render list under a try-shared lock; level unload/stop take the lock exclusively).
- **Run 7 (fade, shading, water):** dither fade across the vanilla overlap band (clipStart/fadeEnd packed into `u_model[0][3][0]`); face shade is baked into vertex colours as in Java (direction index now also carried in `StageAVertex.u`, shader factor off by default); translucent water through a blended `DH_TerrainTransparent` clone of the same compiled material. See `docs/spikes/m1-lods.md`.

### Parallel scratch generation (BdsWorldGenerator)

- Scratch tasks run on the generator's own FIFO worker threads (`common.worldGenerator.scratchThreads`: 0 = auto `max(2, cores-2)`, -1 = legacy single worker doing everything on the server thread). Only `LevelChunk` construction (reads Dimension/Level) and destruction hop to the server thread; `loadChunk`, the snapshot, the non-air check and block lighting run on the worker. `WorldGenerator::mThreadLocalHeightmapProvider` (InstancedThreadLocalValue) and `OverworldGenerator::generatorHelpersPool` (InstancedThreadLocal) are per-calling-thread, i.e. built for generation workers.
- Order: `GenQueue` already issues one `queue()` per section request (nearest first); the 16 chunks of a request are enqueued together in a FIFO, so a section is completed before the next starts.
- If the terrain turns bad (nonAir 0 / garbage) with threads > 0, set `scratchThreads=-1` to confirm it is the off-thread call.
