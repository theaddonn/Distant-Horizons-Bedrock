// Shared by dh_terrain.vert.hlsl and dh_terrain_raster.vert.hlsl: picks the lighting path from u_model[0][3][3] (m)
// and builds the flat texCoords varying for the FS.
// uv0 (TEXCOORD0) = (face encoding (face + 1) / 8 in [0.125, 0.75], packed light (sky*16+block)/255); see
// dh::encodeFaceU. uv0.x == 0 means the TEXCOORD0 stream did not reach the shader (missing vertex field reads 0):
// then the light falls back to full sky (lit by day instead of black) and the debug paint turns magenta.
//   m > 1.5        unlit (terrain unlit-test 2, clouds 3; y = 2 marks it). m > 2.5 = the cloud draw: the VS also
//                  decodes u_model[0][3][0] as the packed tint instead of clip/fade (Run 13b); m > 3.5 (cloud
//                  marker 4) also drops back faces (dhCloudBackFace, Run 14)
//   0.5 <= m       lightmap texture bound at t0: (x, y) = uv0 passed through (y = packed light)
//   m < -2.5       debug paint: x = 3 (uv0 delivered) or 4 (uv0 missing), y = -(1 + packed light)
//   otherwise      procedural lightmap: x = daylight = -m, y = -(1 + packed light)
float2 dhLightVarying(float m, float2 uv0) {
    const bool  uvOk   = uv0.x > 0.06;
    const float packed = uvOk ? uv0.y : 240.0 / 255.0; // missing uv0: sky 15, block 0
    if (m > 1.5) return float2(-1.0, 2.0);
    if (m >= 0.5) return float2(uv0.x, packed);
    if (m < -2.5) return float2(uvOk ? 3.0 : 4.0, -(1.0 + packed));
    return float2(saturate(-m), -(1.0 + packed));
}

// DH cloud layer back-face cull (Run 14). Cloud marker 4 (camera-relative draw, camera outside the cloud slab):
// a cloud face whose plane the camera is behind is dropped, like vanilla, which only builds the faces visible from
// the camera's side of the layer (LevelRendererPlayer::mLastCloudSide). The material has no back-face culling and
// the blend is unsorted, so without this the bottom faces and the far inner walls of every box showed through the
// alpha-0.8 top and stacked into a near-opaque white block. Face (uv0.x = (face + 1) / 8): 0 down, 1 up, 2 north
// (-Z), 3 south (+Z), 4 west (-X), 5 east (+X). All vertices of a quad lie in one plane, so the test agrees for
// the whole quad. A missing uv0 stream (x == 0) never culls.
bool dhCloudBackFace(float2 uv0, float3 camRelPos) {
    if (uv0.x < 0.06) return false;
    const int face = (int)(uv0.x * 8.0 + 0.5) - 1;
    float3 n = float3(1.0, 0.0, 0.0);
    if (face == 0) n = float3(0.0, -1.0, 0.0);
    else if (face == 1) n = float3(0.0, 1.0, 0.0);
    else if (face == 2) n = float3(0.0, 0.0, -1.0);
    else if (face == 3) n = float3(0.0, 0.0, 1.0);
    else if (face == 4) n = float3(-1.0, 0.0, 0.0);
    // camera at the origin: it sees the front side when dot(n, origin - P) > 0
    return dot(n, camRelPos) >= 0.0;
}
