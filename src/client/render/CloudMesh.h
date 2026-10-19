#pragma once

// Pure (no Bedrock headers) cloud geometry: the vanilla clouds.png pattern -> DH quad buffers (64 bytes per quad, see
// dh/platform/IVertexSink.h) that the Stage-A MeshUploader turns into an mce::Mesh. Host-testable
// (tests/unit/cloud_mesh_test.cpp).

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

} // namespace dh::render::cloud
