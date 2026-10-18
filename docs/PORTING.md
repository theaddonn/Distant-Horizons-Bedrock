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
