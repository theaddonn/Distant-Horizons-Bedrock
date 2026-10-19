# Spike: vanilla terrain lighting (RenderChunk) and lightmap generation

Status: research only. Sources: local unpacked RenderChunk material (v26, 1.26.51) disassembled with
`~/.cache/dh-tools/dxc/bin/dxc -dumpbin` (SM60 DXIL variants only; SM40/50 are DXBC and dxc cannot dump them),
plus public GitHub sources named inline.

## 1. RenderChunk sampling convention

### 1a. Fragment (local, `unpacked/RenderChunk/Opaque/0.Direct3D_SM60.Fragment.dxbc`)
- Inputs: COLOR0 (vertex colour/AO), TEXCOORD0.xy (atlas uv, centroid), TEXCOORD1.zw (lightmap uv),
  COLOR2.w (fog factor), TEXCOORD2.y (flag, `> 0.5` enables seasons/alpha blend branch).
- Resources: t0 = `s_LightMapTexture`, t1 = `s_MatTexture` (atlas). Only cbuffer: `FogColor`.
- The lightmap uv is passed straight to the sample: no scale, swizzle or bias in the fragment shader.
- Net colour: `rgb = mix(atlas.rgb * vertexColor.rgb * lightmap.rgb, FogColor.rgb, fog)`.
  The lightmap is a plain multiplier (no separate sky/block tint, no ambient add, no gamma in the fragment).
- Same thing in readable form, veka0/mcbe-shader-codebase `materials/RenderChunk/Opaque.Fragment.glsl` (lines 63, 98):
  https://github.com/veka0/mcbe-shader-codebase/blob/main/materials/RenderChunk/Opaque.Fragment.glsl
  ```
  in highp vec2 v_lightmapUV;
  bgfx_FragData0 = vec4(mix(vec4(texture(s_LightMapTexture, v_lightmapUV).xyz * var_d9690.xyz, var_d9690.w).xyz, FogColor.xyz, vec3(var_26180.w)), var_d9690.w);
  ```

### 1b. Vertex: how v_lightmapUV is built
Local SM60 vertex disassembly (`Opaque/0.Direct3D_SM60.Vertex.dxbc`): `a_texcoord1.xy * 65535.0`, `round`, to uint;
`bit 8` of each component feeds the dithering/mask-tint flags; `(y >> 4) & 15` and `y & 15` times `0x3FB1111120000000` (= 1/15).
Readable (veka0 `Opaque.Vertex.glsl` lines 120-135):
```
uvec2 var_6d79f = uvec2(round(a_texcoord1 * 65535.0));
v_ditheringAndMaskTinting = vec2(notEqual((var_6d79f & uvec2(256u)), uvec2(0u)));
v_lightmapUV = vec2(uvec2(var_5e4ed.y >> 4u, var_5e4ed.y) & uvec2(15u)) * vec2(0.066666670143604278564453125);
```
So only `a_texcoord1.y` (unorm16, n = round(y*65535)) carries light:
- `v_lightmapUV.x = ((n >> 4) & 15) / 15`  = BLOCK light (torch)
- `v_lightmapUV.y = ( n       & 15) / 15`  = SKY light
- `a_texcoord1.x` is unused for light (bit 8 = dither flag).
Block=x / sky=y is confirmed by consumers: Newb X `lighting.h` uses `uv1.x` for torch attenuation (`NL_TORCHLIGHT_INTENSITY*uv1.x`)
and `uv1.y` for sky (`uv1.y/(1.0+lum)`, `step(0.93, uv1.y)`), and Newb's vertex.sc comment says "bits 8-5 for x, bits 4-1 for y":
https://github.com/devendrn/newb-x-mcbe/blob/main/src/materials/RenderChunk/vertex.sc ,
https://github.com/devendrn/newb-x-mcbe/blob/main/src/newb/functions/lighting.h .
It also matches Java where the lightmap row is sky and column is block (see 2).

Caveat on texel addressing: uv = n/15 hits 0.0 and 1.0 exactly, but a 16-wide texture has texel centres at (i+0.5)/16.
Whether the Bedrock lightmap sampler is point or linear (and whether the image is 16x16 or padded) is NOT shown by the shader.
Check the sampler flags of `s_LightMapTexture` in our working binding path before assuming `texelFetch(n)` is equivalent.

## 2. Lightmap generation

### 2a. Java reference (1.18.1 mojmap, `LightTexture.updateLightTexture`, `ClientLevel.getSkyDarken`, `DimensionType.fillBrightnessRamp`)
https://github.com/jacobo-mc/mc_1.18.1_src/blob/main/src/main/java/net/minecraft/client/renderer/LightTexture.java
```
skyDarken = ClientLevel.getSkyDarken(pt):           // 0.2 (night) .. 1.0 (day)
  t = 1 - clamp(1 - (cos(timeOfDay*2pi)*2 + 0.2), 0, 1);  t *= 1 - rain*5/16; t *= 1 - thunder*5/16;  return t*0.8 + 0.2
skyFactor = skyFlashTime > 0 ? 1.0 : skyDarken*0.95 + 0.05        // f1
tint = lerp((skyDarken, skyDarken, 1.0), (1,1,1), 0.35)           // vector3f
blockMul = blockLightRedFlicker + 1.5                             // f4 (flicker is a small random walk, ~0.0 +- 0.1)
ramp(l) = lerp(ambientLight, (l/15)/(4 - 3*(l/15)), 1.0)         // ambientLight: overworld 0.0, nether 0.1
for sky i, block j:
  s = ramp(i)*skyFactor;  b = ramp(j)*blockMul
  c = (b, b*((b*0.6+0.4)*0.6+0.4), b*(b*b*0.6+0.4))               // warm block tint
  if effects.forceBrightLightmap (End): c = lerp(c, (0.99,1.12,1.0), 0.25)
  else: c += tint*s;  c = lerp(c, (0.75,0.75,0.75), 0.04);  [boss darken: lerp(c, c*(0.7,0.6,0.6), amt)]
  c = clamp(c,0,1)
  if nightVision/conduit n>0: m = max(c.rgb); if m<1: c = lerp(c, c/m, n)
  c = lerp(c, 1-(1-c)^4, options.gamma);  c = lerp(c, 0.75, 0.04);  c = clamp(c,0,1)
```
Night-time minimum (Java, gamma 0): skyDarken=0.2 -> skyFactor=0.24, tint=(0.48+0.65*... ) i.e. lerp((.2,.2,1),(1,1,1),.35)=(0.48,0.48,1.0);
at sky=15,block=0 colour ~ (0.24*0.48, 0.24*0.48, 0.24) then the 0.04 lerp to 0.75 adds ~0.03: a bluish ~0.14-0.27 floor.
Block=0,sky=0 is c=0 then +0.03 (0.75*0.04) from the final lerp(s): near black, not pure 0.

### 2b. HLSL-ready pseudocode (Java-equivalent, gamma=0, no night vision)
```hlsl
float ramp(float l)  { float f = l/15.0; return f/(4.0-3.0*f); }        // overworld, ambientLight = 0
float3 lightmap(float block, float sky, float skyDarken /*0.2..1*/) {
  float  skyFactor = skyDarken*0.95 + 0.05;
  float3 tint = lerp(float3(skyDarken, skyDarken, 1.0), 1.0.xxx, 0.35);
  float  b = ramp(block)*1.5;                       // flicker term taken as 0
  float  s = ramp(sky)*skyFactor;
  float3 c = float3(b, b*((b*0.6+0.4)*0.6+0.4), b*(b*b*0.6+0.4));
  c += tint*s;
  c = lerp(c, 0.75.xxx, 0.04);
  c = saturate(c);
  return lerp(c, 0.75.xxx, 0.04);                   // second 0.04 lerp that Java applies after gamma (gamma=0)
}
```
`skyDarken` input: Java's `getSkyDarken` already includes the 0.2 floor. `Dimension::getSkyDarken` in Bedrock
(LeviLamina `src/mc/world/level/dimension/Dimension.h`: `float getSkyDarken(float) const;`, member `mSkyDarken` of type `Brightness`)
is the analogous accessor; its numeric range (0..1 vs 0.2..1, or a 0..15 Brightness) is unverified, see open questions.

### 2c. What is known about Bedrock
- Bedrock builds the texture on the CPU in per-dimension image builders: `LightTextureImageBuilderFactory::registerLightImageBuilders`
  and `registerCustomDimensionLightImageBuilder` (strings in our binary, `strings_all.txt` ~line 1037811), class
  `OverworldLightTextureImageBuilder : BaseLightTextureImageBuilder`
  (https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/client/world/level/dimension/OverworldLightTextureImageBuilder.h, an empty stub: no members or formulas).
  The 16x16 image is then bound as `s_LightMapTexture` (confirmed by the material's `samplerDefinitionMap`).
- Bedrock's numeric curve, sky tint, night floor and night-vision/gamma handling were NOT found in any public source
  and not reverse engineered here (time-boxed). Do not assume Java's constants are identical; they are a best first guess.
- `ViewRenderData::mSkyBrightnessScalar` exists (float, offset-listed in
  https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/deps/minecraft_renderer/objects/ViewRenderData.h)
  next to `mSkyColor/mSkyZenithColor/mSkyHorizonColor`, but no source explains its use. Our repo already observed it reads 0
  (commit be16aab), so use `Dimension::getSkyDarken`.

## 3. Open questions
1. Real Bedrock curve: run a one-off dump of the live lightmap pixels (or hook the image builder) at noon, midnight, rain and
   compare to the Java pseudocode; fit constants (the 0.75 lerp, 1.5 block multiplier, tint) if different.
2. Range and meaning of Bedrock `Dimension::getSkyDarken` (0..1 darkness vs brightness; Java's 0.2 floor present?).
3. Sampler state of `s_LightMapTexture` (point/linear, clamp) and whether uv = n/15 maps to texel centres; affects whether
   shader-side maths should be evaluated at exactly n or interpolated between n-0.5.. texels.
4. Brightness (gamma) slider and night vision: where Bedrock applies them (image builder vs elsewhere); our shader ignores both.
5. Nether/End/custom dimensions: Java uses ambientLight 0.1 and a different End branch; Bedrock has separate builders per
   dimension (Overworld, others unseen). Block-light flicker is also unmodelled.
6. SM40/SM50 DXBC blobs were not disassembled (dxc rejects them); behaviour inferred from SM60 and veka0's readable GLSL.
