#pragma once

// Stage-A render backend, pure part: converts DH 16-byte-vertex quad buffers (see platform/IVertexSink.h) into
// the float vertices the Bedrock backend uploads through mce::MeshData. No Bedrock / LeviLamina headers so it can be
// unit-tested on the host (tests/unit/mesh_stagea_vertex_test.cpp).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dh {

/// 24 bytes. `rgba` packs r | g<<8 | b<<16 | a<<24 (r in the lowest byte); the Bedrock backend repacks it into the
/// engine's native colour order (ColorPacking). Positions are SECTION-LOCAL blocks (the section origin goes into the
/// world matrix).
struct StageAVertex {
    float         x, y, z;
    std::uint32_t rgba;
    float         u, v;
};
static_assert(sizeof(StageAVertex) == 24);

enum class Primitive : std::uint8_t {
    QuadList,     ///< 4 vertices per DH quad, DH vertex order (engine expands quads)
    TriangleList, ///< 6 vertices per DH quad: (0,1,2) (0,2,3)
};

struct ConvertParams {
    /// 0 = full night / sky fully darkened, 1 = full day. Scales the sky-light channel only (see lightFactor).
    float skyBrightness = 1.0f;
    /// Reverses the winding of every quad (Bedrock's front-face convention is not verified; R1 decides).
    bool flipWinding = false;
    /// Also emits the reversed-winding copy of every quad (2x vertices). Use when culling hides the geometry.
    bool doubleSided = false;
    /// Micro offset in blocks applied to the quad corners (DH vertex shader `uMircoOffset`).
    float microOffset = 0.01f;
};

/// CPU stand-in for the lightmap texture. L = max(sky * skyBrightness, block) in light levels (0..15); result =
/// floor + (1 - floor) * 0.8^(15 - L) with floor = 0.04. 0.8^n mirrors the vanilla light-level curve; exactly 1.0 at
/// L=15.
float lightFactor(int sky, int block, float skyBrightness);

inline constexpr std::size_t verticesPerQuad(Primitive p, bool doubleSided = false) {
    return (p == Primitive::QuadList ? 4u : 6u) * (doubleSided ? 2u : 1u);
}

/// Appends the converted vertices to `out` and returns the number of quads converted. A trailing partial quad (buffer
/// size not a multiple of 64) is ignored. `sectionDetail` is accepted for forward compatibility (LOD sections are
/// already in block units, so it does not affect positions). `yOffsetBlocks` is added to every y (level min height).
std::size_t convertDhBuffer(
    std::span<const std::byte> dhQuads,
    int                        sectionDetail,
    float                      yOffsetBlocks,
    std::vector<StageAVertex>& out,
    Primitive                  mode,
    const ConvertParams&       params = {}
);

} // namespace dh
