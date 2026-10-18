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

**Nothing had to be bound.** The wave-0 classes (DhSectionPos, DhChunkPos, DhBlockPos(2D), FullDataPointUtil,
RenderDataPointUtil, FullDataMinMaxPosUtil, VarintUtil, the API enums) have no `SingletonInjector`/`Config` dependency
in their static initialisers; `dumper.Bootstrap.init()` is an empty hook. Later suites that touch config/wrappers
should add their stub bindings there and list them here.

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
`compareDatapointPriority` = `(int)((a>>3) - (b>>3))` (arithmetic shift, 64-bit subtract truncated to int).

### config: one case per enum (`in.enum`), `out`: `count`, `constants[]` {name, ordinal, every instance field}, `methods{}`
EDhApiVerticalQuality (maxVerticalData[] + `calculateMaxNumberOfVerticalSlicesAtDetailLevel_<Q>_detail_-2_to_14`),
EDhApiHorizontalQuality (quadraticBase double + `_bits`, distanceUnitInBlocks), EDhApiMaxHorizontalResolution (dataPointLengthCount, dataPointWidth,
detailLevel, startX/startZ/endX/endZ arrays, `getSelfAndLowerDetails_<R>`), EDhApiWorldGenerationStep (value, name, `fromValue_-3_to_11`),
EDhApiBlockMaterial (index, `getFromIndex_-1_to_17`), EDhApiDistantGeneratorMode (complexity), EDhApiGeneratorPlan (generationEnabled,
chunkGenEnabled, surfaceGenEnabled), EDhApiDataCompressionMode (value, `getFromValue_-1_to_5`), EDhApiWorldCompressionMode (value, `getFromValue_-1_to_2`).
