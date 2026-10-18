# golden-dumper

Runs the ORIGINAL Distant Horizons (Java) core and dumps golden test vectors to `tests/golden/<suite>.gv.json`
so the C++ port can be checked bit-for-bit.

    tools/golden-dumper/run.sh              # build + regenerate everything
    tools/golden-dumper/run.sh pos datapoint  # selected suites (a name also selects its shards pos_1, pos_2 ... if sharding is enabled)
    SKIP_BUILD=1 tools/golden-dumper/run.sh   # reuse last build

Env overrides: `DH_ROOT` (default `/home/lucy/Projects/Code/distant-horizons`, git `2206fe7a4`),
`JAVA_HOME` (default JDK 21), `DH_MC_VER` (default `1.20.1`).
Never modifies the DH repo (gradle writes only its own gitignored `build/` dirs there).

## Bootstrap (what is needed)

1. `build.sh` runs DH's own gradle wrapper with `init.gradle` (`-I`), which registers a `:core:printGoldenClasspath`
   task: it compiles `:api` + `:core` (needs `-PmcVer=...`, any value works; DH's `settings.gradle` requires one) and writes
   core classes + api jar + runtime deps + the `compileOnly` deps (log4j, fastutil, joml, netty, guava, ...)
   to `build/classpath.txt`. First run needs network (buildSrc deps); later runs work with `--offline`.
2. `javac` (JDK 21) compiles `src/main/java/dumper/*.java` against that classpath into `build/classes`.
3. `run.sh` runs `dumper.Main` with the same classpath. DH core classes are Java 8 bytecode; run on JDK 21 is fine.

**Wave-0 suites needed no bindings** (their classes have no `SingletonInjector`/`Config` dependency). The wave-1 suites need the bindings listed under "Bindings" below, set up by `Bootstrap.init()` (which `Main` always runs).

Important build property: DH `ModInfo.VERSION = "3.3.5-dev"` so `ModInfo.IS_DEV_BUILD == true`, which turns ON
`FullDataPointUtil.RUN_VALIDATION` and `RenderDataPointUtil.RUN_VALIDATION`. Validation failures
(`DataCorruptedException`, `AssertFailureException`) are therefore part of the golden behaviour; see "throws" below.

## File format

    {"suite":"pos","javaCommit":"2206fe7a4","generator":"golden-dumper 0.1","cases":[{"name":..,"in":{..},"out":{..}}, ...]}

* Java `long` -> `"0x%016x"` string (two's complement bits); long arrays -> arrays of such strings.
* `int/short/byte` -> JSON numbers (a `byte` in a signed form, e.g. a light value -128..127). `boolean`, `String` plain.
* `double` -> JSON number (shortest round-trip repr, parse with strtod); enum doubles also carry `<name>_bits` hex.
* Any operation that threw is recorded as `{"throws":"<ExceptionSimpleName>"}` in place of its value. The C++ side
  should either reproduce the rejection or skip those keys; they mark exactly which inputs Java rejects.
* Byte strings (varint bytes, netty encoding) are lowercase hex strings without prefix (`"8001"`).
* Deterministic: `java.util.Random(<fixed seed per suite/shard>)`; two runs give byte-identical files.
* Default is one file per suite (total ~8.9 MB). Sharding support is kept: set `SHARDS`/`TOTAL` in `PosSuite`,
  `DatapointSuite` or `RdpSuite` to get `<suite>`, `<suite>_1`, ... with identical schema. Adding a suite = one class implementing `Suite`
  + one `reg(...)` line in `Main`.

**For C++ test authors:** cases whose value is `{"throws": "..."}` must be mapped to the C++ `try*`/optional rejection (or skipped if the C++ API has no equivalent).

## Suites and keys

### pos: `DhSectionPos`, 2,500 cases `sec_N` + 9 hand-checkable `fixed_N` (3.5 MB)
`in`: `detail`(0..127) `x` `z`(28-bit signed incl. extremes) `targetDetail` `relDetail` `returnDetail` `outDetail`
`other`(long, a section pos) `blockX` `blockY` `blockZ` `chunkX` `chunkZ`.
`out` (method -> key): `encode(byte,int,int)`->`encode`; `getDetailLevel/getX/getZ`; `getParentPos`;
`getChildByIndex(pos,0..3)`->`getChildByIndex_0..3`; `getChildIndexOfParent`; `getAdjacentPos(pos,dir)` ->
`getAdjacentPos_{DOWN,UP,NORTH,SOUTH,WEST,EAST}` (UP/DOWN always throw); `convertToDetailLevel(pos,targetDetail)`;
`getMinCornerBlockX/Z`; `getCenterBlockPosX/Z`; `getBlockWidth`; `getChunkWidth`;
`getWidthCountForLowerDetailedSection(pos,returnDetail)`; `contains(pos,other)`->`contains_pos`;
`contains(pos,DhBlockPos)`->`contains_block`; `getDhSectionRelativePositionForDetailLevel(pos,relDetail)`;
`getChebyshevSignedBlockDistance(pos,blockX,blockZ)`; `getManhattanBlockDistance(pos,DhBlockPos2D)`;
`encodeContaining(outDetail,DhBlockPos)`->`encodeContaining_block`; `encodeContaining(outDetail,DhChunkPos)`->`encodeContaining_chunk`.
Note `contains` etc. are computed with Java int overflow for large details; detail is ~60% in 0..24 and 40% in 0..127.
Quirks visible in data: `BitShiftUtil.powerOfTwo(n)` is `1 << n` (shift count mod 32); `DETAIL_LEVEL_MASK` is 0x7F.

### pos_misc (5,006 cases, 1,000 per sub-kind, 2.3 MB)
* `chunk_N` (1000) `DhChunkPos(x,z)`; `in`: x z otherX otherZ blockX blockY blockZ vecX vecZ(double);
  `out`: hashCode, centerBlockPos{x,y,z}, minCornerBlockPos{x,y,z}, getMinBlockX/Z, getMaxBlockX/Z, getMinBlockPos{x,z},
  contains_block, contains_block2d, distance, squaredDistance (doubles), ctor_blockPos{x,z}, ctor_blockPos2d{x,z}, ctor_vec3d{x,z}.
* `block_N` (1000) `DhBlockPos`; `in`: x y z dx dy dz otherX otherY otherZ; `out`: hashCode (`Objects.hash`),
  `createOffset_{DOWN..EAST}`{x,y,z}, `createOffset_xyz`, `createChunkRelativePos`, `getManhattanDistance_3d/_2d`, `encode_hex` (netty, 3x big-endian int).
* `block2d_N` (1000) `DhBlockPos2D`; `in`: x z otherX otherZ; `out`: hashCode, dist (double), distSquared (long), chebyshevDist, manhattanDist, add{x,z}, subtract{x,z}.
* `minmax_dir_<DIR>` (6) `getEncodedMinMaxPos` (+ decoded minX/maxX/minZ/maxZ; UP/DOWN throw) and `minmax_N` (1000):
  `in`: minX maxX minZ maxZ (shorts) raw (long); `out`: encodeAdjMinMaxPos, getAdjMinX/MaxX/MinZ/MaxZ of it and `raw_*` of `raw`.
  Quirk: negative shorts sign-extend into the long (`(long) minX << 0`).
* `varint_N` (1000): `in`: value; `out`: `writeVarint_hex` (negative throws), `zigzagEncode`, `zigzagDecode_of_value`,
  `zigzagRoundTrip`, `writeVarint_zigzag_hex` (writeVarint of zigzagEncode(value), throws if that is negative).
  `readVarint` is not dumped (round-trip is covered by DH's own VarintTest).

### datapoint: `FullDataPointUtil`, 2,500 cases `dp_N` (1.6 MB)
`in`: id height minY blockLight skyLight (bytes as ints) raw(long) newId newHeight newMinY newBlockLight newSkyLight.
`out`: `encode(id,height,minY,byte blockLight,byte skyLight)` (NB: blockLight before skyLight) then, if it succeeded,
`encoded_getId/getHeight/getBottomY/getBlockLight/getSkyLight`; for `raw`: `getId getHeight getBottomY getBlockLight getSkyLight`,
`setId setHeight setBottomY setBlockLight setSkyLight` (applied to raw with the `new*` args), `validateDatapoint`.
Layout: id bits 0-31 (mask 0x7FFFFFFF), height 32-43, minY 44-55, skyLight 56-59, blockLight 60-63.
Quirks observed: `setId/setHeight/setBottomY/setBlockLight/setSkyLight` do not mask the new value before OR-ing, so out-of-range
or negative values corrupt neighbouring/higher fields (negative sign-extends to the top). In particular `setBlockLight(data, byte)`
(and setSkyLight) parenthesise as `(data & ~mask) | value<<off` with no mask on `value`. `encode` with minY 4096..6094 passes validation
but fails the round-trip assertion (`AssertFailureException`).

### rdp: `RenderDataPointUtil`, 2,000 `rdp_N` + 1,000 `raw_N` (1.6 MB)
`rdp_N` `in`: alpha red green blue maxY(0..6094, truncated to 12 bits when packed) minY lightSky lightBlock materialId color(argb int) newMinY other(long);
`out`: `createDataPoint(alpha,r,g,b,maxY,minY,sky,block,mat)`, `createDataPoint_color(maxY,minY,color,sky,block,mat)` (overload; may throw),
and if packing succeeded: `getYMax getYMin getAlpha getRed getGreen getBlue getLightSky getLightBlock getBlockMaterialId getColor
doesDataPointExist hasZeroHeight isVoid_derived setYMin compareDatapointPriority compareDatapointPriority_rev` (`isVoid_derived` = exists && hasZeroHeight;
no `isVoid` exists in DH).
`raw_N`: same getters for a random 64-bit `raw` with `other`. `getAlpha` is `(a4 << 4) | 0xF` (4-bit alpha), shorts/bytes dumped as ints.
`compareDatapointPriority` = `(int)((a>>7) - (b>>7))` (arithmetic shift, 64-bit subtract truncated to int).

### config: one case per enum (`in.enum`), `out`: `count`, `constants[]` {name, ordinal, every instance field}, `methods{}`
EDhApiVerticalQuality (maxVerticalData[] + `calculateMaxNumberOfVerticalSlicesAtDetailLevel_<Q>_detail_-2_to_14`),
EDhApiHorizontalQuality (quadraticBase double + `_bits`, distanceUnitInBlocks), EDhApiMaxHorizontalResolution (dataPointLengthCount, dataPointWidth,
detailLevel, startX/startZ/endX/endZ arrays, `getSelfAndLowerDetails_<R>`), EDhApiWorldGenerationStep (value, name, `fromValue_-3_to_11`),
EDhApiBlockMaterial (index, `getFromIndex_-1_to_17`), EDhApiDistantGeneratorMode (complexity), EDhApiGeneratorPlan (generationEnabled,
chunkGenEnabled, surfaceGenEnabled), EDhApiDataCompressionMode (value, `getFromValue_-1_to_5`), EDhApiWorldCompressionMode (value, `getFromValue_-1_to_2`).

---

# Wave 1 suites (headless wrappers required)

## Bindings (what the real code demanded)

All done in `Bootstrap.init()` / `Extra.bindAll()`; `Main` calls it once before any suite.

| Binding | Why | Stub |
|---|---|---|
| `SingletonInjector` <- `IWrapperFactory` | static init of `FullDataPointIdMap`, `LodDataBuilder` | `Stub` proxy: `getAirBlockStateWrapper`=block 0, `getWaterBlockStateWrapper`=block 4, `getPlainsBiomeWrapper`=biome 0, `deserialize*Wrapper` parse `B<n>` / `M<n>` |
| `SingletonInjector` <- `IMinecraftSharedWrapper` | `ConfigHandler` resolves `<installDir>/config/DistantHorizons.toml` on first `Config` access | proxy; `getInstallationDirectory()` = `tools/golden-dumper/build/scratch` (wiped every run, so every Config value starts at its default); then `ConfigHandler.tryRunFirstTimeSetup()` |
| (same stub) `supportsSurfaceGeneration()` = true | `WorldGenPlanConfigEventHandler` would otherwise rewrite surface generator plans to CHUNKS_ONLY | `val("supportsSurfaceGeneration", true)` |
| `SingletonInjector` <- `IMinecraftRenderWrapper` | static init of `DebugWireframeHandler` | all-zero proxy |
| `SingletonInjector` <- `DebugWireframeHandler` | static init of `DhLightingEngine` | the real `DebugWireframeHandler.INSTANCE` |
| `Config.Common.LodBuilding.worldCompression` | read by `updateFromDataSource` and `createFromChunk` | set per case via `.set(...)` (recorded in `in.worldCompression`: 0 = MERGE_SAME_BLOCKS, 1 = VISUALLY_EQUAL) |
| `ILevelWrapper` / `IClientLevelWrapper` passed to `createFromChunk` | only forwarded to the (unused) API event | all-zero proxy |

`Stub` (dynamic proxy) returns 0/false/null for every method that is not explicitly implemented; only the methods the code under test calls are implemented.

## Headless block table (`block_table.gv.json`)

Palettes, chunks etc. reference blocks by serial; wrapper equality is by serial. `getSerialString` = `B<serial>`, biomes `M<serial>` (4 biomes, 0..3).

| serial | name | isAir | opacity | isSolid | isLiquid | lightEmission | materialId | isIce |
|---|---|---|---|---|---|---|---|---|
| 0 | air | 1 | 0 | 0 | 0 | 0 | 14 | 0 |
| 1 | stone | 0 | 16 | 1 | 0 | 0 | 2 | 0 |
| 2 | dirt | 0 | 16 | 1 | 0 | 0 | 5 | 0 |
| 3 | grass_block | 0 | 16 | 1 | 0 | 0 | 13 | 0 |
| 4 | water | 0 | 3 | 0 | 1 | 0 | 12 | 0 |
| 5 | ice | 0 | 3 | 1 | 0 | 0 | 0 | 1 |
| 6 | glass | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| 7 | leaves | 0 | 1 | 1 | 0 | 0 | 1 | 0 |
| 8 | torch | 0 | 0 | 0 | 0 | 14 | 3 | 0 |
| 9 | snow_layer | 0 | 2 | 0 | 0 | 0 | 8 | 0 |
| 10 | poppy | 0 | 0 | 0 | 0 | 0 | 13 | 0 |
| 11 | glowstone | 0 | 16 | 1 | 0 | 15 | 15 | 0 |
| 12 | lava | 0 | 16 | 0 | 1 | 15 | 6 | 0 |
| 13 | stairs | 0 | 16 | 0 | 0 | 0 | 3 | 0 |
| 14 | deepslate | 0 | 16 | 1 | 0 | 0 | 7 | 0 |
| 15 | cave_air | 1 | 0 | 0 | 0 | 0 | 14 | 0 |
| 16 | sand | 0 | 16 | 1 | 0 | 0 | 9 | 0 |

(opacity 16 = fully opaque, `LodUtil.BLOCK_FULLY_OPAQUE`; 0 = fully transparent.) Two distinct air ids (0, 15) exist on purpose: code must treat every block that is air / opacity 0 alike.

## Data source JSON (`Ds`)

    {"pos":"0x..","palette":[{"block":serial,"biome":serial},...],
     "defGen":int,"defComp":int,                         (dumps only; inputs omit them = 0)
     "cols":[[index,genStep,compMode,[longs...]],...]}     sparse, ascending index = relX*64 + relZ

Palette position = datapoint id. Datapoints are `FullDataPointUtil` longs, index 0 = highest datapoint. Unlisted columns have no datapoints, `genStep = defGen`, `compMode = defComp`. Dumps additionally carry `"isEmpty"`. genStep is `EDhApiWorldGenerationStep.value` (-1 down-sampled, 0 empty .. 9 light); compMode is `EDhApiWorldCompressionMode.value`.
Note: `createWithData` sources have `isEmpty=false`; an input whose palette (mapping) is empty makes `updateFromDataSource` return false immediately.

## merge_same / merge_up / downsample
`in`: `{worldCompression, target:<Ds>, inputs:[<Ds>...]}`; `updateFromDataSource(input)` is applied for each input in order (merge_same, downsample: 1 input; merge_up: the 4 children in `getChildByIndex` order 0..3, each with its own palette). merge_same d in 6..8; merge_up children at detail d, parent d+1; downsample: input is the parent (d+1), target the child (random child index of the parent).
`out`: `{dataChanged:[bool per input], target:<Ds dump after all calls>}` (after `removeUnusedIdsAndRemap` and, if worldCompression=1, `FullDataOcclusionCuller` on all columns with more than 1 datapoint).
Quirks the data covers: merge_up majority vote ignores air; light is averaged only over sub-columns with the winning id (integer division); parent genStep = min of the 4 children, compMode = max of the 4 (signed byte); downsample writes genStep -1 into ALL 4096 columns, reads the input compMode at the *recipient* index, and replaces a column only if the target column has no non-zero datapoint.

## chunk_build
`in {worldCompression, lit, chunk{chunkX,chunkZ,minY,maxY,profiles,colRuns,biomeRuns}}`; `out {pos,palette,genStep,compMode,isEmpty,applyToParent,offsetX,offsetZ,colTable,colRuns,otherNonEmpty}`.
- Chunk encoding: `profiles[p]` = run-length `[[blockSerial,count],...]` of one column from minY upward; `colRuns` = run-length of profile index over the 256 columns, order `x*16+z`; `biomeRuns[qx*4+qz]` = run-length `[[biome,count],...]` over 4-block-high cells from minY (biome looked up at cell `(x>>2, clamp((y-minY)>>2), z>>2)`).
- `DumpChunk` mirrors `ChunkWrapper`: heightmaps from `createDhHeightMaps` (solid = first `isSolid`, light-blocking = first `opacity != 0`, scan down from the top of the highest non-empty 16-block section, value = world Y of that block, `minNonEmptyHeight` if none); sections are empty iff all blocks `isAir`; blocks outside [minY,maxY) are air; light via the real `ChunkLightStorage` (sky: 15 above maxY, 0 below minY, 0 inside if never set).
- `lit=true`: `DhLightingEngine.lightChunk(chunk, [chunk], 15)` ran first (single chunk, no neighbours); `lit=false`: no DH light data (block 0, sky 0 inside the height range).
- Out: `colRuns` = run-length of `colTable` index over the chunk's 256 columns (`x*16+z`, data source column = `(offsetX+x)*64 + offsetZ+z`, `offset = (chunk & 3) * 16`); `genStep`/`compMode` are uniform (9 / worldCompression). The topmost air datapoint is 1 taller than the build height (`lastY - y`, y = the block below) - real Java behaviour, visible in every fixture.

## cull
`in {source:<Ds>}`; the loop `for x, for z: if column.size() > 1 cullHiddenDatapointsInColumn(source,x,z)` is applied; `out {changed:[[index,[longs]],...]}` = only the columns whose datapoints differ afterwards.

## light_ds
`in {maxSkyLight, source:<Ds>}`; `out {cols:[[index,[longs]],...]}` (the columns listed in `in.source.cols`, same order, after `bakeDataSourceSkyLight`).

## reduce
`RenderDataPointReducingList` driven through `ColumnRenderView.changeVerticalSizeFrom`. One case = one render column; the result for every target size is in `out`.
`in {column:[longs]}`: `RenderDataPointUtil` datapoints of one column, index 0 = highest, 1..64 points, possible implicit gaps (air), invisible points (alpha nibble 0, `isAlphaVisible` = `getAlpha >= 16`) and translucent ones.
`out {t1,t2,t3,t4,t6,t8,t16}`: each an array of exactly that many longs (the target column; unused slots are 0). Source = view with `maxVerticalSliceCount = n`, 1 sub-view; target = view with `maxVerticalSliceCount = t`. `t >= n` is a plain copy (zero padded); `t == 1` is `reduceToOne`; otherwise populate/reduce/copyTo. The reduction order depends on fastutil's (unstable) `Arrays.quickSort` (fastutil 8.2.1) when sizes tie - the C++ port has to reproduce that exact sort.

## quads
`LodQuadBuilder` (client level = `DumpClientLevel`: `getBlockColor` = `0xFF000000 | ((b*0x9E3779B1) ^ (m*0x85EBCA6B)) & 0xFFFFFF`, `getDirtBlockColor` = that for block 2 / biome 0, `getShade`: UP 1.0, DOWN 0.5, NORTH/SOUTH 0.8, WEST/EAST 0.6).
`in {doTransparency, grassSideRendering(0 AS_GRASS,1 FADE_TO_DIRT,2 AS_DIRT), debugRendering(EDhApiDebugRendering ordinal), earthCurveRatio(0|100 -> max quad width 16 instead of 2048), showOverlappingQuadErrors, calls[]}` with
`["adj",dir,x,y,z,width,height,color,faceTexId,materialId,skyLight,blockLight]` (dir = `EDhDirection` ordinal: 0 DOWN, 1 UP, 2 NORTH, 3 SOUTH, 4 WEST, 5 EAST; DOWN/UP not used), `["up"|"down",x,y,z,blockWidth,color,faceTexId,materialId,skyLight,blockLight]`. Colors are signed ARGB ints. Calls are applied in order, then `mergeQuads()`.
`out {opaqueQuads,transparentQuads,opaque:[hex],transparent:[hex]}`: counts after the merge; one hex string per ByteBuffer returned by `makeOpaque/TransparentVertexBuffers` (6 directions in faceIndex order, quads in list order after sorting/merging, 4 vertices x 16 bytes, **little-endian** because the buffers use native byte order): `i16 x,y,z; u16 meta = sky | block<<4 | microOffset<<8; u8 r,g,b,a; u8 materialId; u8 normalIndex; u16 textureTileId`. Grass (material 13) side/bottom colour switches to the dirt colour (shaded) depending on `grassSideRendering` (only when debugRendering == 0). Each case covers exactly one (doTransparency, grassMode) combination, cycling through all 6.

## light_chunk
`in {maxSkyLight, centerChunkX, centerChunkZ, chunks:[{dx,dz,chunk:<ChunkFx json as in chunk_build>}]}` -> `DhLightingEngine.INSTANCE.lightChunk(center, listInGivenOrder, maxSkyLight)` (centre is included in the list, absent neighbours are omitted).
`out {skyCorrect, blockCorrect, colTable:[[[v,count],...]], colRuns:[[tableIdx,count],...]}`: light of the centre chunk only, per column (index `x*16+z`) run-length over y = minY..maxY-1 of `v = sky*16 + block` (the stored `ChunkLightStorage` values; sections that were never written read 0 - including sky light above the highest non-empty section, which is only reached by propagation, exactly like the real code).
World height used: minY 0, maxY 64 (one fixture -64..320).

## treemath (2,300 cases: `detail_N` x1300, `cone_N` x300, `frustum_N` x700)
- `detail_N` - `LodQuadTree` detail-level maths. The tree is created *without* its constructor (`Unsafe.allocateInstance`); only `blockRenderDistanceDiameter` and `treeRootDetailLevel = max(max(1,6), MathUtil.log2(diameter))` (as in `QuadTree`) are set, the private `updateDetailLevelVariables()` is run against the `Config` values, then the (private) `calcDetailLevelFromDistance` and public `calcExpectedDetailLevel` are called.
  `in {horizontalQuality (EDhApiHorizontalQuality ordinal), maxHorizontalResolution (EDhApiMaxHorizontalResolution ordinal), diameter, player:[x,z], distances:[double x8], sections:[long x6], blocks:[[x,z] x4], zoom: null | {magnification, coneTanHalfAngle, lookX, lookZ}}`
  `out {dropOffUnit (= distanceUnitInBlocks*16), dropOffLogBase (= ln(quadraticBase)), maxLeafDetail, minRootDetail, treeRootDetail, fromDistance[8], expectedSection[6], expectedBlock[4]}`. `fromDistance[i] = clamp(maxLeaf, (int)(ln(d/unit)/logBase), ROOT_SECTION_DETAIL_LEVEL)` (note `(int)` truncation toward zero, NaN/-inf -> clamp). `expectedSection/Block` use `CameraZoom.INSTANCE` (set from `zoom`; sections with detail >= 12 never zoom).
- `cone_N` - `CameraZoom.coneIntersectsCircle`: `in {coneTanHalfAngle, lookX, lookZ, origin:[x,z], circles:[[cx,cz,r] x8]}` `out {hits:[bool x8]}`.
- `frustum_N` - `DhFrustumBounds.update` + `intersects` (JOML 1.10.2 `FrustumIntersection`/`Matrix4f.frustumAabb`). The matrix is in `DhApiMat4f` order (`mat[r*4+c]` = `m<r><c>`, i.e. row-major; DH transposes into JOML).
  `in {minY, maxY, mat:[16 floats], matBits:[16 int bit patterns], boxes:[[minX,minZ,width,detail] x12]}`
  `out {planeNames:[24], planes:[24 floats], planeBits:[24], boundsMin:[x,y,z], boundsMax:[x,y,z], visible:[bool x12]}`: the 24 floats are JOML's private plane fields in declaration order (nx, px, ny, py, nz, pz; each X,Y,Z,W - normalised planes), `bounds*` is `invert(viewProj).frustumAabb` and `visible[i] = testAab((minX,minY,minZ),(minX+w,maxY,minZ+w))`. All float maths is IEEE single precision; `*Bits` fields give the exact bit patterns.

## net (442 cases)
Message cases `close_reason_N, level_init_N, request_level_init_N, session_config_N, cancel_N, exception_N, data_request_N, data_response_N, partial_update_N, split_N` (24 each) + `codec_crash_0/1`, and `session_constrain_N` (200).
`out {id, body, frame, roundTrip}` (hex strings, Netty big-endian): `body` = `message.encode(buf)`; `frame` = `i16 16 (PROTOCOL_VERSION)` + `u16 messageId` + body (the non-forge path of `AbstractPluginPacketSender.encodeMessage`); `roundTrip` = decoding `body` into a fresh instance and re-encoding gives the same bytes and consumes everything. Message ids: 1 CloseReason, 2 LevelInit, 3 RequestLevelInit, 4 SessionConfig, 5 Cancel, 6 Exception, 7 FullDataSourceRequest, 8 FullDataSourceResponse, 9 FullDataPartialUpdate, 10 FullDataSplit, 11 CodecCrash (dev builds only; encode throws for phase 0, decode always throws -> `out.throws` / `out.decodeThrows`).
Trackable messages (5..8 only; 9 and 10 are not trackable) start with `i32 (int) futureId`. Strings are `u16 byteLength + UTF-8`. `in` keys: close_reason `{reason}`; level_init `{dimensionResourceLocation, serverKey, levelKey, serverTime(long)}`; request_level_init `{dimensionResourceLocation}`; session_config `{values:[12]}`; cancel `{futureId}`; exception `{futureId, exceptionType(0 RateLimited,1 RequestOutOfRange,2 RequestRejected,3 SectionRequiresSplitting), message}`; data_request `{futureId, levelName, sectionPos, clientTimestamp|null}` (optional = `bool` + `i64`); data_response `{futureId, payload|null}`; partial_update `{levelName, payload}`; payload = `{dtoBufferId, beams:[[x,y,z,rgb24]]}` encoded as `i32 id` + `i32 count` + per beam `i32 x,y,z` + `i32 argb` (alpha 255); split `{bufferId, buffer:hex, isFirst}` -> `i32 id, i32 len, bytes, bool`.
SessionConfig wire format = 12 values in this fixed order, no count: generatorPlan (1 byte ordinal), maxGenerationRequestDistance (i32), generationCenterChunkX (i32), generationCenterChunkZ (i32), generationMaxChunkRadius (i32), generationRequestRateLimit (i32), enableRealTimeUpdates (bool), realTimeUpdateDistanceRadiusInChunks (i32), synchronizeOnLoad (bool), maxSyncOnLoadRequestDistance (i32), syncOnLoadRateLimit (i32), playerBandwidthLimit (i32). `session_config_N.in.values` is that order (the plan as ordinal).
`session_constrain_N`: `in {a:[12 values], b:[12 values]}` (the frozen configs A and B, same order) -> `out {aConstrainedByB:[12], bConstrainedByA:[12], aConstrainedByB_encoded}` where "X constrained by Y" = `X.constrainingConfig = Y` and reading the 12 getters; every entry is `constrainer(Y.value, X.value)` with: generatorPlan = DISABLED if either is DISABLED, else the first plan (enum order, generation enabled) with the same surface/chunk flags as the AND of both, else X's own plan; maxGenerationRequestDistance, generationRequestRateLimit, realTimeUpdateDistanceRadiusInChunks, maxSyncOnLoadRequestDistance, syncOnLoadRateLimit = min; generationCenterChunkX/Z, generationMaxChunkRadius = X's own value; enableRealTimeUpdates, synchronizeOnLoad = AND; playerBandwidthLimit = 0 if both are 0, else min over the values that are > 0 (a 0 counts as "unlimited" = Integer.MAX_VALUE). The getter result order equals the wire order above.

## genpos (400 cases)
`GeneratedFullDataSourceProvider.getPositionsToRetrieve(pos, generatorDetailLevel)` with the provider created without its constructor and a stubbed `FullDataSourceV2Repo` (`existsWithKey`, `getColumnGenerationStepForPos`).
`in {plan (EDhApiGeneratorPlan ordinal), pos, generatorDetailLevel, hasQueue, repo:[{pos, steps:[[genStep,count],...] run-length over the 4096 columns (index relX*64+relZ), [] = row exists but has no column data}]}`; positions not listed in `repo` do not exist.
`out {positions: [longs] | null}` - null when `hasQueue=false`; otherwise the positions to generate in `forEachChildAtDetailLevel` child order (empty list = nothing to generate / fully generated). Required step: SURFACE_ONLY -> SURFACE(5); plan has surface generation and the pos is above `SECTION_BLOCK_DETAIL_LEVEL` (= 6, i.e. detail > 6) -> SURFACE; otherwise FEATURES(8).
Config: `generatorPlan` is set through `Config.Common.WorldGenerator.generatorPlan.set()`; `IMinecraftSharedWrapper.supportsSurfaceGeneration()` is stubbed to `true` so `WorldGenPlanConfigEventHandler` does not rewrite the plan.

## Notes for suite authors
- Never touch `Config` (directly or through a static initialiser/field of a suite class) before `Bootstrap.init()` has run: `WorldGenPlanConfigEventHandler`, `ConfigHandler` etc. capture singletons in static initialisers.
- Direct `SingletonInjector` bindings can only be added in `Bootstrap`/`Extra`; per-suite stubs use `Stub.of(Interface.class).on("method", args -> value).build()`.
