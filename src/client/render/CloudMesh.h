#pragma once

// Pure (no Bedrock headers) cloud geometry: the vanilla clouds.png pattern -> DH quad buffers (64 bytes per quad, see
// dh/platform/IVertexSink.h) that the Stage-A MeshUploader turns into an mce::Mesh. Host-testable
// (tests/unit/cloud_mesh_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dh::render::cloud {

/// 2D boolean cloud pattern that tiles infinitely. Cell (i, j) = texel (i mod w, j mod h): i along +X, j along +Z.
struct Pattern {
    int                       w = 0, h = 0;
    std::vector<std::uint8_t> solid; ///< w * h, 1 = cloud

    bool empty() const { return w <= 0 || h <= 0; }
    bool at(long i, long j) const {
        long x = i % w, z = j % h;
        if (x < 0) x += w;
        if (z < 0) z += h;
        return solid[static_cast<std::size_t>(z) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] != 0;
    }
    std::size_t count() const;
};

/// Cloud where alpha > 127 (vanilla clouds.png: white texels, alpha 255 = cloud, alpha 1 = clear).
Pattern patternFromRgba(std::uint8_t const* rgba, int w, int h);
/// Deterministic 256x256 blob pattern used when clouds.png cannot be read.
Pattern proceduralPattern();

/// Locates file `name` inside a Bedrock `.brarchive` (the vanilla packs ship textures as
/// `__brarchive/textures/environment.brarchive`, which holds clouds.png). Layout (1.26.51, read from the file):
/// u64 magic 0x267052A0B125277D | u32 count | u32 version (1) | count x 256-byte entries {u8 nameLen,
/// name[247], u32 offset, u32 size} | data; offsets are relative to the end of the entry table. Sets [off, off+len)
/// to the file bytes and returns true when found and in bounds.
bool findInBrArchive(std::uint8_t const* data, std::size_t n, char const* name, std::size_t& off, std::size_t& len);

struct BuildParams {
    int          cell    = 12;             ///< blocks per texel
    double       centerX = 0, centerZ = 0; ///< cloud-space centre of the covered square (blocks)
    double       halfExtent = 1024;        ///< half side of the covered square (blocks, <= 30000)
    int          anchorX = 0, anchorZ = 0; ///< mesh-local origin (cloud-space blocks); int16 local coordinates
    float        yBottom   = 0;            ///< y of the cloud bottom in the converted space (before the min-y offset)
    float        thickness = 4;            ///< blocks (fancy boxes)
    bool         fancy     = true;         ///< true: boxes (top, bottom, sides); false: one flat layer
    std::uint8_t r = 255, g = 255, b = 255, a = 204;
};

struct Built {
    std::vector<std::vector<std::byte>> buffers; ///< whole quads, each <= kMaxBufferBytes
    std::size_t                         quads = 0;
};

/// Greedy-merged top/bottom rectangles plus run-merged side walls. Face shading as Java DH's cloud shading
/// (top 1.0, bottom 0.7, north/south 0.9, east/west 0.8) is baked into the vertex colour. Sky light 15.
Built build(Pattern const& pattern, BuildParams const& p);

/// Face group indices (also the vertex normal field and the DH_Terrain face encoding): 0 down, 1 up, 2 north (-Z),
/// 3 south (+Z), 4 west (-X), 5 east (+X).
inline constexpr int kFaceGroups = 6;
/// Tiles are fixed squares of kTileCells cells in cloud space (tile (ti, tj) = cells [ti * kTileCells, ...)), so
/// camera movement only adds/removes tiles. Local coordinates (<= kTileCells * cell) stay far inside int16.
inline constexpr int kTileCells = 96;

struct TileParams {
    int          cell = 12;
    int          ti = 0, tj = 0;
    float        yBottom   = 0;
    float        thickness = 4;
    bool         fancy     = true;
    std::uint8_t r = 255, g = 255, b = 255, a = 204;
};

/// One tile as six per-face-group quad lists (index = face), coordinates local to the tile's min corner
/// (ti * kTileCells * cell, tj * kTileCells * cell). Same quads and shading as build() over the tile's cell square.
std::array<Built, 6> buildTile(Pattern const& pattern, TileParams const& t);

/// Horizontal distance from (px, pz) to the rectangle [x0, x1] x [z0, z1] (0 inside).
double rectDistance(double x0, double z0, double x1, double z1, double px, double pz);

/// Bit mask (1 << face) of the face groups of a tile that the VS back-face cull (cloud marker 4, dhCloudBackFace)
/// drops entirely for a camera at (camX, camY, camZ); the tile spans [minX, maxX] x [minZ, maxZ] horizontally and
/// the slab [slabBottom, slabTop] vertically (all in the same space as the camera). Only valid for a camera outside
/// the slab and camera-relative draws, like marker 4 itself.
unsigned backFaceGroupMask(
    double camX,
    double camY,
    double camZ,
    double minX,
    double maxX,
    double minZ,
    double maxZ,
    double slabBottom,
    double slabTop
);

} // namespace dh::render::cloud
