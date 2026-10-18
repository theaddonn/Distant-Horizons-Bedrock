#pragma once

// Port of core/dataObjects/render/bufferBuilding/LodQuadBuilder.java (GL, pooling and
// thread-local plumbing removed). Config / IMinecraftRenderWrapper / IClientLevelWrapper
// reads are replaced by QuadBuilderOptions.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dh/mesh/BufferQuad.h"
#include "dh/mesh/MeshBuffers.h"
#include "dh/pos/Direction.h"
#include "dh/render/Enums.h"

namespace dh {

inline constexpr int kBytesPerVertex = 16;
inline constexpr int kBytesPerQuad   = kBytesPerVertex * 4;
/// 2 MiB rounded down to a whole number of quads (LodQuadBuilder.getMaxBufferByteSize).
inline constexpr std::size_t kMaxBufferByteSize = (2 * 1024 * 1024 / kBytesPerQuad) * kBytesPerQuad;

struct QuadBuilderOptions {
    /// LodQuadBuilder.getBuilder(doTransparency, ...): Complete == doTransparency.
    Transparency transparency = Transparency::Disabled;
    /// Config...Quality.grassSideRendering.
    GrassSideRendering grassSide = GrassSideRendering::AsGrass;
    /// Config...Debugging.debugRenderingColors != OFF.
    bool debugRenderMode = false;
    /// IClientLevelWrapper.getDirtBlockColor() (ARGB).
    std::uint32_t dirtColorArgb = 0xFF000000u;
    /// IClientLevelWrapper.getShade(dir), indexed by Direction faceIndex (Down,Up,North,South,West,East).
    std::array<float, 6> shadeByFace = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    /// Config...Experimental.earthCurveRatio != 0 (read in BufferQuad.tryMerge).
    bool earthCurveEnabled = false;
    /// Config...Debugging.showOverlappingQuadErrors (read in BufferQuad.tryMerge).
    bool showOverlappingQuadErrors = false;
};

class LodQuadBuilder {
public:
    explicit LodQuadBuilder(const QuadBuilderOptions& options = {});

    /// Horizontal faces only; Down is rejected (Java throws) and returns false.
    /// Returns false (quad dropped) for a non-positive width/height.
    bool addQuadAdj(
        Direction     dir,
        std::int16_t  x,
        std::int16_t  y,
        std::int16_t  z,
        std::int16_t  width,
        std::int16_t  height,
        std::uint32_t argb,
        std::uint16_t tile,
        std::uint8_t  material,
        std::uint8_t  skyLight,
        std::uint8_t  blockLight
    );
    bool addQuadUp(
        std::int16_t  minX,
        std::int16_t  maxY,
        std::int16_t  minZ,
        std::int16_t  blockWidth,
        std::uint32_t argb,
        std::uint16_t tile,
        std::uint8_t  material,
        std::uint8_t  skyLight,
        std::uint8_t  blockLight
    );
    bool addQuadDown(
        std::int16_t  x,
        std::int16_t  y,
        std::int16_t  z,
        std::int16_t  blockWidth,
        std::uint32_t argb,
        std::uint16_t tile,
        std::uint8_t  material,
        std::uint8_t  skyLight,
        std::uint8_t  blockLight
    );

    /// Greedy merge (stable sorts); UP/DOWN get a second pass.
    void mergeQuads();

    /// Packs all quads: faces in index order, each buffer <= kMaxBufferByteSize, split on quad boundaries.
    MeshBuffers build() const;

    int getCurrentOpaqueQuadsCount() const;
    /// 0 when transparency is disabled (as Java).
    int getCurrentTransparentQuadsCount() const;
    int getCurrentQuadsCount() const { return getCurrentOpaqueQuadsCount() + getCurrentTransparentQuadsCount(); }
    /// Not in Java: number of vertex buffers build() will return (opaque + transparent).
    int getCurrentNeededVertexBufferCount() const;
    int getPremergeCount() const { return premergeCount_; }

    const std::vector<BufferQuad>& opaqueQuads(Direction d) const { return opaque_[dir::faceIndex(d)]; }
    const std::vector<BufferQuad>& transparentQuads(Direction d) const { return transparent_[dir::faceIndex(d)]; }

    /// DIRECTION_VERTEX_IBO_QUAD[face][vertex] = {a, b}
    static const int kVertexIbo[6][4][2];

private:
    QuadBuilderOptions                     options_;
    bool                                   doTransparency_;
    std::array<std::vector<BufferQuad>, 6> opaque_, transparent_;
    int                                    premergeCount_ = 0;

    bool add(std::vector<BufferQuad>& list, const BufferQuad& quad, bool tryPremerge);
    long mergeInternal(std::array<std::vector<BufferQuad>, 6>& lists, int faceIndex, MergeDirection md);
    std::vector<std::vector<std::byte>> makeBuffers(const std::array<std::vector<BufferQuad>, 6>& lists) const;
    void                                putQuad(std::vector<std::byte>& buf, const BufferQuad& quad) const;
    void                                putVertex(
                                       std::vector<std::byte>& buf,
                                       std::int16_t            x,
                                       std::int16_t            y,
                                       std::int16_t            z,
                                       std::uint32_t           color,
                                       std::uint8_t            normalIndex,
                                       std::uint8_t            material,
                                       std::int8_t             sky,
                                       std::int8_t             block,
                                       std::uint16_t           tile,
                                       int                     mx,
                                       int                     my,
                                       int                     mz
                                   ) const;
};

} // namespace dh
