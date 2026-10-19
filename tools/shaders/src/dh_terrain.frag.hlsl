// Spike B3: DH_Terrain fragment shader (see dh_terrain.vert.hlsl for the signature).
// FS uniform table (we define it): DhParams vec4 (fragment) @0, cbuf 16 B, plus the sampler s_MatTexture (slot 0 = t0/s0)
// which the renderer binds to LevelRenderer::mBrightnessTex (the vanilla 16x16 lightmap: u = block light, v = sky light,
// refreshed per frame for day/night/gamma). Light arrives in texCoords.y (uv0.y, see dh_terrain_light.hlsli) = (sky*16+block)/255 (CPU packLight; the
// lightmapSwapAxes option swaps the nibbles, i.e. the lookup axes).
// light.x = near clip radius (discard below it; 0 = off), light.w = fade end (dither fade between x and w), light.y/z =
// fog start/end in blocks, fog.rgb = vanilla's current fog colour. Output: vertex colour * DhParams.rgb, far fog.
Texture2D LightMap : register(t0);
SamplerState LightSamp : register(s0);

cbuffer FragmentUniforms : register(b1) {
    float4 DhParams;
};

struct PSIn {
    float4 pos : SV_Position;
    float4 clipPosition : COLOR3;
    float4 color : COLOR0;
    float4 fog : COLOR2;
    float4 light : COLOR1;
    nointerpolation float2 texCoords : TEXCOORD0; // see dh_terrain_light.hlsli (flat: per-quad)
    float3 worldPos : TEXCOORD3;
};

// Face shade by DH direction index (down, up, north, south, west, east) = Java ClientLevelWrapper.getShade. Java bakes
// it into the vertex colour on the CPU (ColumnMeshBuilder / LodQuadBuilder do the same here), so it is OFF by default;
// set to 1 only if the CPU path is changed to leave colours unshaded (otherwise it would be applied twice). The face is
// only in texCoords.x on the lightmap-texture path (x = (face + 1) / 8): with the define on, the shader /
// debug modes (x = daylight or 3/4) would decode garbage, so it is valid with a bound lightmap only.
#define DH_SHADE_IN_SHADER 0

// Ordered 4x4 Bayer matrix in [0,1), as the Java DH shader's bayerMatrix4x4.
float bayer4x4(float2 p) {
    static const float m[16] = {0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};
    int2 q = int2(fmod(p, 4.0));
    return m[q.y * 4 + q.x] / 16.0;
}

float4 main(PSIn i) : SV_Target0 {
    // Java frag.frag:135 `viewDist = length(vertexWorldPos)`: 3D distance to the camera, not horizontal - from high
    // up the ground below is far away and must render (vanilla culls its sub-chunks by 3D distance too).
    float d = length(i.worldPos);
    float clipStart = i.light.x, fadeEnd = i.light.w;
    if (clipStart > 0.0) {
        if (d < clipStart) discard;
        // dither fade across the overlap band with vanilla terrain (Java DH near-fade behaviour)
        if (fadeEnd > clipStart && d < fadeEnd) {
            float t = (d - clipStart) / (fadeEnd - clipStart);
            if (bayer4x4(i.pos.xy) > t) discard;
        }
    }
    float3 c = i.color.rgb * DhParams.rgb;
    // lightmap lookup (Java frag.frag: texture(uLightMap, lightMapUV) * vertexColor). Texel centres of a 16x16 map.
    // DhParams.w < 0.5 disables lighting (fullbright vertex colour). texCoords.y (see the VS): >= 1.5 unlit (clouds);
    // 0..1 packed light with a lightmap texture bound at t0; < 0 shader lighting: packed = -y - 1, daylight = x.
    if (DhParams.w >= 0.5 && i.texCoords.y < 1.5) {
        if (i.texCoords.y >= 0.0) {
            float  packedLight = round(saturate(i.texCoords.y) * 255.0);
            float  hi = floor(packedLight / 16.0), lo = packedLight - hi * 16.0;
            float2 dim;
            LightMap.GetDimensions(dim.x, dim.y);
            float2 uv = (float2(lo, hi) / 15.0 * (dim - 1.0) + 0.5) / dim; // texel centres for any lightmap size (16 -> (l+0.5)/16)
            c *= LightMap.SampleLevel(LightSamp, uv, 0).rgb;
        } else {
            // Procedural lightmap (no vanilla texture available): Java LightTexture.updateLightTexture, gamma 0, no
            // night vision (docs/spikes/vanilla-lighting.md 2b). daylight = skyDarken (0.2 night .. 1 day).
            // Packed light = sky * 16 + block.
            float  packedLight = round(saturate(-i.texCoords.y - 1.0) * 255.0);
            float  sky = floor(packedLight / 16.0), block = packedLight - sky * 16.0;
            // debug paint (x > 2.5): R = sky/15, G = block/15, B = 0.25 (uv0 delivered) or 1.0 (x = 4: TEXCOORD0
            // missing, light forced to sky 15 by the VS -> bright magenta)
            if (i.texCoords.x > 2.5) return float4(sky / 15.0, block / 15.0, i.texCoords.x > 3.5 ? 1.0 : 0.25, 1.0);
            float  skyDarken = clamp(i.texCoords.x, 0.2, 1.0);
            float  fs = sky / 15.0, fb = block / 15.0;
            float  rampS = fs / (4.0 - 3.0 * fs), rampB = fb / (4.0 - 3.0 * fb);
            float  skyFactor = skyDarken * 0.95 + 0.05;
            float3 tint = lerp(float3(skyDarken, skyDarken, 1.0), float3(1.0, 1.0, 1.0), 0.35);
            float  b = rampB * 1.5;
            float  s = rampS * skyFactor;
            float3 l = float3(b, b * ((b * 0.6 + 0.4) * 0.6 + 0.4), b * (b * b * 0.6 + 0.4));
            l += tint * s;
            l = lerp(l, float3(0.75, 0.75, 0.75), 0.04);
            l = lerp(l, float3(0.75, 0.75, 0.75), 0.04);
            l = saturate(l);
            c *= l;
        }
    }
#if DH_SHADE_IN_SHADER
    static const float shade[6] = {0.5, 1.0, 0.8, 0.8, 0.6, 0.6};
    c *= shade[(int)clamp(round(i.texCoords.x * 8.0) - 1.0, 0.0, 5.0)];
#endif
    float fogStart = i.light.y, fogEnd = i.light.z;
    float a = i.color.a;
    if (fogEnd > fogStart) {
        float f = saturate((d - fogStart) / (fogEnd - fogStart));
        c = lerp(c, i.fog.rgb, f);
        // unlit blended geometry (the DH cloud layer, texCoords.y >= 1.5) also fades out, so the square edge of the
        // cloud mesh dissolves into the sky instead of ending as a fog-coloured border
        if (i.texCoords.y >= 1.5) a *= 1.0 - f;
    }
    return float4(c, a);
}
