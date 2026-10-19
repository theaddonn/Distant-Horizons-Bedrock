// Spike B3: DH_Terrain fragment shader (see dh_terrain.vert.hlsl for the signature).
// FS uniform table (we define it): DhParams vec4 (fragment) @0, cbuf 16 B. No texture sampling.
// light.x = near clip radius (discard below it; 0 = off), light.w = fade end (dither fade between x and w), light.y/z =
// fog start/end in blocks, fog.rgb = vanilla's current fog colour. Output: vertex colour * DhParams.rgb, far fog.
cbuffer FragmentUniforms : register(b1) {
    float4 DhParams;
};

struct PSIn {
    float4 pos : SV_Position;
    float4 clipPosition : COLOR3;
    float4 color : COLOR0;
    float4 fog : COLOR2;
    float4 light : COLOR1;
    centroid float2 texCoords : TEXCOORD0;
    float3 worldPos : TEXCOORD3;
};

// Face shade by DH direction index (down, up, north, south, west, east) = Java ClientLevelWrapper.getShade. Java bakes
// it into the vertex colour on the CPU (ColumnMeshBuilder / LodQuadBuilder do the same here), so it is OFF by default;
// set to 1 only if the CPU path is changed to leave colours unshaded (otherwise it would be applied twice).
#define DH_SHADE_IN_SHADER 0

// Ordered 4x4 Bayer matrix in [0,1), as the Java DH shader's bayerMatrix4x4.
float bayer4x4(float2 p) {
    static const float m[16] = {0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};
    int2 q = int2(fmod(p, 4.0));
    return m[q.y * 4 + q.x] / 16.0;
}

float4 main(PSIn i) : SV_Target0 {
    float d = length(i.worldPos.xz);
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
#if DH_SHADE_IN_SHADER
    static const float shade[6] = {0.5, 1.0, 0.8, 0.8, 0.6, 0.6};
    c *= shade[(int)clamp(i.texCoords.x + 0.5, 0.0, 5.0)];
#endif
    float fogStart = i.light.y, fogEnd = i.light.z;
    if (fogEnd > fogStart) c = lerp(c, i.fog.rgb, saturate((d - fogStart) / (fogEnd - fogStart)));
    return float4(c, i.color.a);
}
