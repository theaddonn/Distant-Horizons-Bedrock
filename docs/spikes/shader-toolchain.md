# Spike B0: offline `.material.bin` toolchain

Status: complete (offline only; the game directory was only read, never written).
Target: Minecraft Bedrock 1.26.51 (GDK), `data/renderer/materials/*.material.bin`, format
`RenderDragon.CompiledMaterialDefinition` **version 26**, encryption `NONE`
(156 of 157 files; `RTXStub` uses `SimplePassphrase`).

## 1. Summary

| Question | Answer |
|---|---|
| Does MaterialBinTool (master = v0.9.1, `508dba7`) read v26? | **No, not as shipped.** It silently produces 0 passes. Two missing fields found; a 70-line patch (`tools/shaders/matbintool-v26.patch`) makes it work. |
| Unpack / repack | Works after patch. **Repack is byte-identical for 156/157 files** (all except `RTXStub`, which re-randomises its AES IV). |
| Python reader | `tools/shaders/matbin_inspect.py` (stand-alone, parses all 156 unencrypted files incl. trailer-magic check). |
| Blob container | **bgfx shader binary v5 (`VSH\x05` / `FSH\x05`)** wrapping a DXBC container (SM40/50: legacy DXBC; SM60/65: DXBC container with a `DXIL` part). Layout fully decoded; `bgfx_build()` re-serialises all 2800+ blobs of 5 materials byte-exactly. |
| `dxc` (SM6.5 DXIL) | Linux release v1.9.2609 downloaded; compiles `vs_6_5`/`ps_6_5`; output is a signed `DXBC` container with parts `SFI0 ISG1 OSG1 PSV0 STAT HASH DXIL` (identical part list to vanilla SM65 blobs). |
| `fxc` (SM5 DXBC) | No `fxc.exe` on the box, **but** `tools/shaders/fxc_lite.c` (mingw, calls `D3DCompile` from wine's `d3dcompiler_47.dll`) compiles `vs_4_0/vs_5_0/ps_5_0` under wine. Caveat: it is wine's vkd3d-based compiler, not Microsoft's. |
| bgfx `shaderc` | Not installed. Not needed: we assemble the v5 container ourselves. |

## 2. Reproduce

```
tools/shaders/build_mbt.sh                      # clone + patch + javac (no gradle: its 6.7.1 wrapper dies on JDK 21)
M=~/.steam/steam/steamapps/common/MinecraftBedrockGDK/builds/Retail-1.26.5101-Levi/data/renderer/materials
J=/usr/lib/jvm/java-21-openjdk-amd64/bin/java; JAR=~/.cache/dh-tools/mbt-build/mbt.jar
$J -jar $JAR -u $M/RenderChunk.material.bin -o ~/.cache/dh-tools/unpacked   # -> RenderChunk/RenderChunk.json + <Pass>/<n>.<Platform>.<Stage>.dxbc
$J -jar $JAR -r ~/.cache/dh-tools/unpacked/RenderChunk -o ~/.cache/dh-tools/repacked
cmp ~/.cache/dh-tools/repacked/RenderChunk.material.bin $M/RenderChunk.material.bin   # identical
tools/shaders/roundtrip_check.sh $M             # all materials: identical=156 different=1 (RTXStub)
python3 -I tools/shaders/matbin_inspect.py info|dump|extract|bgfx|selftest ...
```

Unpacked output (RenderChunk): 1838 files, 17 MB, in `~/.cache/dh-tools/unpacked/` (not committed). Note the tool's
"unpack" writes the raw bgfx blobs with a `.dxbc` extension for all four D3D platforms (SM60/65 contain DXIL).
JDK 21 was used; gradle build is not needed (`build_mbt.sh` compiles the 99 sources with `javac`, deps gson 2.9.1,
jcommander 1.82, jsr305 from Maven Central).

## 3. What changed in v26 (the patch)

MaterialBinTool supports versions >= 22 and ignores unknown trailing structure. Observed v26 deltas:

1. `u16 extra` (always 0) between the property-field table and the pass count.
2. Pass `flagDefaultValues` entries are `{string flag, u16 n, n*string allowedValues}` (a *list* per flag, e.g.
   `Dithering: [Off, On]`) instead of `{flag, value}`. Order of the list is not "default first" in general
   (`UseTextures: [On, Off]`, `AlphaTest: [Off, On_VertexTintMaskBased, On_DiscardValueBased]`).
3. `u32 passExtra` between the pass flag table and the variant count. Value `0` on `DepthOnly*` passes,
   `1268872610` (0x4BA175A2) on every other pass in every file I looked at. Meaning unknown (looks like a constant
   hash/GUID); we copy it verbatim.
4. Input attribute `(8,0)` exists (used by some materials); added to the enum (`Unknown_8_0`), otherwise bulk unpack aborts.

Everything else (sampler layout, property fields, variant/flags/blob layout, `ShaderInput`) is unchanged from the tool's model.

### Complete v26 file layout (little-endian; string = `u32 len` + bytes)

```
u64 0x0A11DA1A | string "RenderDragon.CompiledMaterialDefinition" | u64 version=26 | 4B "ENON" (= "NONE" reversed u32 signature)
string name | bool hasParent [string]
u8 nSamplers { string name | u16 reg | u8 access | u8 precision | bool uav | u8 type | string texFormat
               | u32 unkInt(1) | u8 unkByte (= reg) | bool h2 [u8] | bool hasDefaultTex [string] | bool hasStr [string]
               | bool hasCustom [string, u32] }
u16 nProps   { string name | u16 type (2 vec4, 3 mat3, 4 mat4, 5 external) | [u32 num | bool hasData | 16/36/64 bytes] }
u16 extra (0)
u16 nPasses  { string name | string platformBitset ("1111111111111") | string fallback ("") | bool hasBlend [u16 blendMode]
               | u16 nFlags { string flag | u16 n | n*string value } | u32 passExtra
               | u16 nVariants { bool supported | u16 nFlags | u16 nBlobs | nFlags*{string,string}
                                 | nBlobs*{ string stageName | string platformName | u8 stage | u8 platform
                                            | u16 nInputs { string name | u8 type | u8 attr | u8 sub | bool perInstance
                                                            | bool hasPrec [u8] | bool hasInterp [u8] }
                                            | u64 sourceHash | u32 blobLen | blob } } }
u64 0x0A11DA1A
```

## 4. Material anatomy

Full per-variant dumps: `matbin_inspect.py dump FILE`. Blend state: `hasDefaultBlend = false` in every pass of every
file I checked, and **no depth/cull/stencil/raster state exists in the file at all** (that lives in the engine side:
`.material` JSON states / C++ `MaterialPtr` state flags). So pass/variant selection by flag name is the only
"state" the container carries.

### RenderChunk (2,083,270 B)

- Samplers (all `Type2D`, `Read`, float): `LightMapTexture` reg 0, `MatTexture` reg 1, `SeasonsTexture` reg 2.
  (bgfx sampler uniform names in the blobs are `s_LightMapTexture`, `s_MatTexture`, `s_SeasonsTexture`.)
- Property fields (all vec4; `x N` = array count; default): `DitherParams` (0,0,0,0), `DitherParams2` x3, `GlobalRoughness`
  (.5,.5,.5,.5), `FogAndDistanceControl` 0, `FogColor` (1,1,1,1), `LightDiffuseColorAndIlluminance` (1,1,1,1),
  `LightWorldSpaceDirection` (0,-1,0,1), `MeshContext` 0, `RenderChunkFogAlpha` 0, `SubPixelOffset` 0, `ViewPositionAndTime` 0.
- Passes (12 variants each): `DepthOnlyOpaque`, `AlphaTest`, `DepthOnlyAlphaTest`, `Opaque`, `Transparent`.
  Flags in every pass: `Dithering{Off,On}`, `Instancing{Off,On}`, `RenderAsBillboards{Off,On}`, `Seasons{Off,On}`
  (12 of the 16 combinations are compiled).
- Platforms per variant (8 blobs): `Direct3D_SM40`, `SM50`, `SM60`, `SM65` x {Vertex, Fragment}. Nothing else.
  Blob sizes, Opaque pass (12 variants): SM40 VS 2620-3236 / FS 992-995; SM50 VS 2668-3284 / FS 1012-1023;
  SM60 VS 6777-7560 / FS 5522-5645; SM65 VS 6773-7556 / FS 5538-5653. Other passes: SM65 FS up to ~7.2 KB, VS up to ~8 KB.
- Vertex inputs (Opaque, v0, all four platforms the same; semantic = the container's `(attr,sub)` pair):

  | name | type | semantic | perInstance |
  |---|---|---|---|
  | position | Vec3 | Position | no |
  | color0 | Vec4 | Color0 | no |
  | lightmapUV | Vec2 | TexCoord1 | no |
  | texcoord0 | Vec2 | TexCoord0 | no |
  | instanceData0 / 1 / 2 | Vec4 | TexCoord7 / 6 / 5 | **yes** |

  The instance inputs are listed in every variant (even `Instancing=Off`); bgfx attribute id list differs
  (`[5,1,16,17]` vs `[5,1,16,17,23,22,21]`), see section 5.
- Fragment (= varying) inputs: `color0` Vec4 Color0, `ditheringAndMaskTinting` Vec2 TexCoord2, `fog` Vec4 Color2,
  `lightmapUV` Vec2 TexCoord1, `texcoord0` Vec2 TexCoord0 (CENTROID), `worldPos` Vec3 TexCoord3.
- Engine uniforms actually referenced (SM65, constant buffer layout from the bgfx header):
  - VS: `u_proj` mat4 @0, `u_view` mat4 @64, `u_model` mat4[4] @128 (16 vec4), `FogAndDistanceControl` @384, `FogColor` @400,
    `MeshContext` @416, `RenderChunkFogAlpha` @432, `SubPixelOffset` @448, `ViewPositionAndTime` @464; cbuf 480 B
    (DepthOnly passes: 224 B; some variants omit the tail).
  - FS (Opaque): `FogColor` @0 (cbuf 16 B), samplers; Transparent/AlphaTest FS: `u_view`, `DitherParams2`[3], `DitherParams`, `ViewPositionAndTime` (cbuf 160 B).

### MeshFallbackPosUVNormalColor (5,342,787 B)

- Samplers: `MatTexture` reg 0 (Type2D).
- Props (vec4): `HudOpacity` (1,1,1,1), `AlphaMaskedTint`, `FogControl`, `ChangeColor` (1,1,1,1), `CurrentColor` (1,1,1,1),
  `DiscardValue` (.5,.5,.5,.5), `FogColor` (1,1,1,0), `DitherParams`, `DitherParams2` x3, `DitheringEnabledToggle`, `OverlayColor`,
  `SubPixelOffset`, `TileLightColor` (1,1,1,1), `TileLightIntensity`, `UVAnimation` (0,0,1,1), `ZShiftValue`.
- Passes: `AlphaTest`, `RasterizedTransparent`, `DepthOnlyAlphaTest`, `Opaque`, `RasterizedAlphaTest`, `Transparent`, `RasterizedOpaque`
  (24 variants each). Flags: `AlphaTest{Off,On_VertexTintMaskBased,On_DiscardValueBased}`, `Lit{Off,On}`, `MultiColorTint{Off,On}`, `UseTextures{On,Off}`.
- VS inputs: `position` Vec3 Position, `normal` Vec4 Normal, `color` Vec4 Color0, `texCoords` Vec2 TexCoord0. Varyings: `color` Color0,
  `light` Color1, `fog` Color2, `clipPosition` Color3, `texCoords` TexCoord0 (CENTROID), `worldPos` TexCoord3.
- `Rasterized*` passes use a pre-multiplied `u_viewProj` instead of `u_proj`+`u_view`. `DepthOnly*` fragment blobs have stage `Unknown`
  (empty 390 B DXBC, 0 uniforms, input hash differs from the VS).
- Uniforms used: VS `u_model` mat4[4], `FogControl`, `FogColor`, `UVAnimation`, `ZShiftValue`, `OverlayColor`, `TileLightColor`, `SubPixelOffset`;
  FS `CurrentColor`, `OverlayColor`, `HudOpacity`, `AlphaMaskedTint`, `ChangeColor`, `DiscardValue`, `DitherParams*`, `DitheringEnabledToggle`, `u_view`, `u_invView`.

### Other materials checked (for reference)
`Core/Color` (passes DepthOnlyAlphaTest/DepthOnlyOpaque/Opaque, 2 variants), `FullscreenEffect`
(`FullScreenOverlayTransparent`/`Opaque`/`Transparent`), `Entity` (14 passes `AlphaTest*`, `Glint`, `Emissive`, `MultiColor`, `ColorMask`, ...).
All parse and round-trip.

## 5. Blob container (bgfx shader binary v5) - exact header

Each blob in the `.material.bin` is a *bgfx shader binary, version 5*, which wraps the native shader. Identical for SM40/50/60/65.

```
off  bytes                         meaning
0    56 53 48 05    ("VSH",5)      magic: VSH = vertex, FSH = fragment (CSH compute); version byte 5
4    5f 98 6d 2c                   u32 inputHash (VS and FS of one variant carry the SAME hash = varying-linkage check)
8    03 00                         u16 numUniforms          (v5 has no outputHash; it appears only for version >= 6)
10   08 'FogColor' 12 00 0000 0100 per uniform: u8 nameLen, name, u8 type, u8 num, u16 regIndex, u16 regCount
     ...                             type = kind | 0x10 fragment-only | 0x20 sampler;  kind 0 Sampler, 2 Vec4, 3 Mat3, 4 Mat4
                                     regIndex = BYTE offset into the stage's constant buffer (or texture slot for samplers),
                                     regCount = vec4 slots; num = array size (u_model num=4, regCount=16)
..   u32 codeSize, code[codeSize]  native shader: "DXBC" container (SM40/50: SHDR/SHEX; SM60/65: ... DXIL part)
..   00                            terminator
..   u8 numAttrs, numAttrs*u16     bgfx attribute ids (VS only; 0 for FS), e.g. 05 00 01 00 10 00 11 00  (see below)
..   u16 cbufSize                  constant-buffer byte size (VS full: 480 = 0x1e0, FS Opaque: 16)
```

Real first bytes of `RenderChunk / Opaque / v0 / Direct3D_SM65 / Fragment` (5538 B total, DXBC code 5462 B):
```
46 53 48 05 5f 98 6d 2c 03 00 08 46 6f 67 43 6f 6c 6f 72 12 00 00 00 01 00   FSH 5, hash, 3 uniforms, FogColor(0x12 vec4|frag, reg 0, count 1)
11 's_LightMapTexture' 30 01 00 00 01 00                                      sampler (0x30), num 1, slot 0
0c 's_MatTexture'      30 01 01 00 01 00                                      sampler slot 1
```
and the VS: `56 53 48 05 5f 98 6d 2c 09 00 06 'u_proj' 04 00 0000 0400  06 'u_view' 04 00 4000 0400  07 'u_model' 04 04 8000 1000 ...`.

Observations that matter for building our own:
- SM65/SM60 `code` = real DXBC container, parts `SFI0 ISG1 OSG1 PSV0 STAT HASH DXIL` (exactly what `dxc -T vs_6_5` emits).
  SM40/50 `code` parts `ISGN OSGN SHEX` (vanilla fxc output is stripped of `RDEF`/`STAT`; ours has them - harmless?, unverified).
- The `matbin` per-blob `sourceHash` (u64) is not derivable (probably hash of the source/defines); we can use anything - open question whether the
  engine checks it. The variant `inputs` list per blob must match the shader's signature.
- The bgfx `attributes` ids for RenderChunk VS are `[5,1,16,17]` (+ `23,22,21` for instancing); these are *not* bgfx-stock enum values
  (stock: Position 0, Color0 4, TexCoord0 10...). Probably Mojang-specific; copy from a vanilla variant with the same input set.
- `bgfx_header()/bgfx_build()` in `matbin_inspect.py` round-trip all 480 (RenderChunk), 1344 (MeshFallback), 784 (Entity), 144, 48 blobs byte-exactly.

## 6. Compilers

| Tool | Path / status |
|---|---|
| `dxc` | not on PATH. Downloaded `linux_dxc_2026_09_28.x86_x64.tar.gz` (release v1.9.2609) to `~/.cache/dh-tools/dxc/`; needs `LD_LIBRARY_PATH=~/.cache/dh-tools/dxc/lib` (ships `libdxil.so` so output is signed). |
| `fxc` | none installed (xwin sysroot has no `fxc.exe`; only wine's `d3dcompiler_3x-47.dll`, builtin, ~1 MB). |
| `fxc_lite.exe` | built with `x86_64-w64-mingw32-gcc` (present); `WINEPREFIX=~/.local/share/amtool/wineprefix wine fxc_lite.exe in.hlsl entry vs_5_0 out.bin` works (wine 10.0). |
| bgfx `shaderc` | not present anywhere under `$HOME` (depth 5). |

```
export LD_LIBRARY_PATH=~/.cache/dh-tools/dxc/lib
dxc -T vs_6_5 -E vs_main t.hlsl -Fo t.vs.dxil     # 4408 B, DXBC, parts SFI0 ISG1 OSG1 PSV0 STAT HASH DXIL
dxc -T ps_6_5 -E ps_main t.hlsl -Fo t.ps.dxil     # 4024 B, same parts
WINEPREFIX=... wine fxc_lite.exe t.hlsl vs_main vs_5_0 t5.vs   # 2520 B, parts ISGN OSGN RDEF SHDR STAT
```
- `dxc` rejects `vs_5_0` (it promotes to 6.0), as expected.
- wine's compiler emits `SHDR` for `vs_5_0` (Microsoft's fxc emits `SHEX` for SM5.x) - may be accepted or not by the D3D11 runtime; test in-game only if D3D11 is active.
- bgfx `shaderc` feasibility: its D3D backend (`shaderc_hlsl.cpp`) `dlopen`s `d3dcompiler_47.dll`, so a Linux build cannot target D3D; it would
  have to be built for Windows (mingw/clang-cl via the xwin sysroot) and run under wine, **and** upstream shaderc writes the *current* bgfx
  container (v11+/`FSH` with output hash and texture-info), not Mojang's v5. Mojang's fork is not public. Conclusion: skip shaderc, assemble the
  container ourselves (trivial, verified).

## 7. Repack dry-run

`RenderChunk.material.bin` unpacked then repacked with the patched tool: **0 differing bytes** (2,083,270 B). `MeshFallbackPosUVNormalColor`: identical (5,342,787 B).
Bulk: 156/157 identical; `RTXStub` differs only because SimplePassphrase encryption uses a fresh random IV on every save.
Nothing was copied into the game directory.

## 8. Design note: Stage B pipeline

```
HLSL (hand-written, bgfx-style ports of RenderChunk with our own DH vertex format)
  |-- dxc -T vs_6_5 / ps_6_5   (Linux, libdxil-signed)         -> DXBC container w/ DXIL        [SM65 (and SM60)]
  |-- fxc_lite vs_5_0 / ps_5_0 (wine d3dcompiler_47)            -> DXBC (SHDR)                   [SM50 / SM40 only if D3D11 is active]
  v
tools/shaders (to write): bgfx v5 assembler: "VSH"/"FSH" 5 + inputHash + uniform table + code + 0 + attrs + cbufSize
  - uniform table & cbuf layout: we define them; names/offsets/size must match what the HLSL cbuffer declares
  - uniforms/attribute ids: clone from the vanilla RenderChunk variant with the same input set
  v
Unpacked JSON (clone RenderChunk.json: samplers, props, one pass per needed pass name, variants, inputs per blob; set sourceHash arbitrary)
  v
MaterialBinTool -r (patched)  ->  DH_Terrain.material.bin   (+ entry in materials.index.json if the loader needs it; R-spike decides)
```
Minimum useful platform set: **SM65 + SM60** for D3D12 (DXIL); SM50 + SM40 for D3D11 (vanilla ships all four so the engine picks at runtime).
Cheapest start: take the vanilla `RenderChunk` json, replace only the `Opaque` pass blobs (rebuilt bgfx wrappers around our DXIL), keep every
input list/flag; repack and swap in-game. This keeps the reflection (inputs/uniform tables) vanilla-valid and isolates risk to the shader code.

### Engine-fed uniforms we can reuse by name (all present in RenderChunk/MeshFallback VS/FS)
`u_proj`, `u_view`, `u_model[4]`, `u_viewProj` (Rasterized*), `u_invView`, `FogColor`, `FogAndDistanceControl`, `FogControl`, `ViewPositionAndTime`,
`SubPixelOffset`, `RenderChunkFogAlpha`, `MeshContext`, `DitherParams`, `DitherParams2`[3], `DitheringEnabledToggle`, `CurrentColor`, `ChangeColor`,
`OverlayColor`, `TileLightColor`, `UVAnimation`, `ZShiftValue`, `HudOpacity`, `AlphaMaskedTint`, `DiscardValue`; samplers `s_LightMapTexture`, `s_MatTexture`,
`s_SeasonsTexture`. Which of them the *engine* actually uploads for a custom material depends on the `propertyFields` in our own JSON (same as vanilla).

### Smuggling per-draw parameters
- `u_model` is a mat4[4] (16 vec4 = 256 B): for a non-instanced chunk draw only `u_model[0]` is meaningful; rows/slots 1-3 and the unused 4th
  column of an affine matrix are free carriers for chunk origin, LOD level, fade distances, etc. (to verify what the engine writes into slots 1-3).
- `MeshContext`, `RenderChunkFogAlpha` (RenderChunk) / `CurrentColor`, `ChangeColor`, `UVAnimation`, `ZShiftValue` (MeshFallback) are per-draw vec4 properties:
  a MeshFallback-style material set through the mesh-draw API could carry 3-6 vec4 per draw if we can set them (R-spike).
- Per-instance data: the instanceData0..2 inputs are real per-instance vertex streams (3 x vec4 = 48 B/instance) - the most generous channel if
  instancing is available to our draw path.
- Vertex colour (`color0`, 4 x u8/f32) and `lightmapUV` in the vertex stream can carry per-vertex DH data (block/sky light, biome tint) with no engine change.

### Open questions (in-game spikes)
1. **R0: D3D11 vs D3D12?** Decides SM50/40 (fxc via wine, `SHDR` vs `SHEX`) vs SM65/60 only. Cheap check: loaded modules (`d3d12.dll` vs `d3d11.dll`, `dxil`), or which blob the engine validates.
2. Does the loader verify `sourceHash`, `inputHash`, the `passExtra` constant, `extra` u16s? (we copy them from vanilla; only matters for novel shaders.)
3. Does a new material name need a `materials.index.json` entry, and how are custom materials found (data/renderer/materials only, or also resource packs)? Does anything block unsigned/modified files in the GDK build (file integrity)?
4. How does the material get bound to our draw (frame-graph entry points are opaque in 1.26.51; see plan Stage C)?
5. Which `u_model` slots and properties does the engine really populate for a chunk draw? Is `Instancing=On` selectable from our draw path?
6. Will an SM5 DXBC from wine's compiler (with `RDEF`/`STAT` and `SHDR`) be accepted by the runtime? (Only relevant under D3D11.)
7. bgfx attribute id list `[5,1,16,17,...]` - confirm mapping by comparing with vertex-layout declarations in-game before inventing new inputs.
