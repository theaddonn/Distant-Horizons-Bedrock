#include "dh/mesh/LodQuadBuilder.h"

#include <algorithm>

#include "dh/platform/BlockInfo.h"
#include "dh/util/Color.h"

namespace dh {

// Indexed by Direction::faceIndex (Down=0, Up=1, ...). NOTE: the Java comments label
// entry 0 "UP" and entry 1 "DOWN", but Java indexes by faceIndex (DOWN=0, UP=1), so the
// table is used as-is, i.e. DOWN quads use the entry commented "UP". Preserved.
const int LodQuadBuilder::kVertexIbo[6][4][2] = {
    {{1, 0}, {1, 1}, {0, 1}, {0, 0}}, // face 0 (DOWN)  X,Z
    {{0, 0}, {0, 1}, {1, 1}, {1, 0}}, // face 1 (UP)    X,Z
    {{0, 0}, {0, 1}, {1, 1}, {1, 0}}, // NORTH           X,Y
    {{1, 0}, {1, 1}, {0, 1}, {0, 0}}, // SOUTH           X,Y
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, // WEST            Z,Y
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}}, // EAST            Z,Y
};

LodQuadBuilder::LodQuadBuilder(const QuadBuilderOptions& options)
: options_(options),
  doTransparency_(options.transparency == Transparency::Complete) {}

bool LodQuadBuilder::add(std::vector<BufferQuad>& list, const BufferQuad& quad, bool tryPremerge) {
    if (tryPremerge && !list.empty()) {
        BufferQuad       copy = quad;
        QuadMergeOptions mo{options_.earthCurveEnabled, options_.showOverlappingQuadErrors};
        BufferQuad&      last = list.back();
        if (last.tryMerge(copy, MergeDirection::EastWest, mo)
            || last.tryMerge(copy, MergeDirection::NorthSouthOrUpDown, mo)) {
            ++premergeCount_;
            return true;
        }
        // tryMerge may have flagged `copy` as errored (overlap), which Java keeps.
        list.push_back(copy);
        return true;
    }
    list.push_back(quad);
    return true;
}

bool LodQuadBuilder::addQuadAdj(
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
) {
    if (dir == Direction::Down) {
        return false; // Java throws: only for adjacent directions
    }
    auto&      list = (doTransparency_ && color::getAlpha(argb) < 255) ? transparent_[dir::faceIndex(dir)]
                                                                       : opaque_[dir::faceIndex(dir)];
    BufferQuad quad;
    if (!quad.set(x, y, z, width, height, argb, tile, material, skyLight, blockLight, dir)) {
        return false;
    }
    return add(list, quad, true);
}

bool LodQuadBuilder::addQuadUp(
    std::int16_t  minX,
    std::int16_t  maxY,
    std::int16_t  minZ,
    std::int16_t  blockWidth,
    std::uint32_t argb,
    std::uint16_t tile,
    std::uint8_t  material,
    std::uint8_t  skyLight,
    std::uint8_t  blockLight
) {
    const int  f    = dir::faceIndex(Direction::Up);
    auto&      list = (doTransparency_ && color::getAlpha(argb) < 255) ? transparent_[f] : opaque_[f];
    BufferQuad quad;
    if (!quad.set(
            minX,
            maxY,
            minZ,
            blockWidth,
            blockWidth,
            argb,
            tile,
            material,
            skyLight,
            blockLight,
            Direction::Up
        )) {
        return false;
    }
    return add(list, quad, false);
}

bool LodQuadBuilder::addQuadDown(
    std::int16_t  x,
    std::int16_t  y,
    std::int16_t  z,
    std::int16_t  blockWidth,
    std::uint32_t argb,
    std::uint16_t tile,
    std::uint8_t  material,
    std::uint8_t  skyLight,
    std::uint8_t  blockLight
) {
    const int  f    = dir::faceIndex(Direction::Down);
    auto&      list = (doTransparency_ && color::getAlpha(argb) < 255) ? transparent_[f] : opaque_[f];
    BufferQuad quad;
    if (!quad.set(x, y, z, blockWidth, blockWidth, argb, tile, material, skyLight, blockLight, Direction::Down)) {
        return false;
    }
    return add(list, quad, false);
}

//=======//
// merge //
//=======//

long LodQuadBuilder::mergeInternal(std::array<std::vector<BufferQuad>, 6>& lists, int faceIndex, MergeDirection md) {
    auto& list = lists[static_cast<std::size_t>(faceIndex)];
    if (list.size() <= 1) {
        return 0;
    }
    // Java List.sort == stable TimSort
    std::stable_sort(list.begin(), list.end(), [md](const BufferQuad& a, const BufferQuad& b) {
        return a.compare(b, md) < 0;
    });

    const QuadMergeOptions mo{options_.earthCurveEnabled, options_.showOverlappingQuadErrors};
    long                   mergeCount = 0;
    std::vector<bool>      removed(list.size(), false);
    std::size_t            current = 0;
    for (std::size_t i = 1; i < list.size(); ++i) {
        if (list[current].tryMerge(list[i], md, mo)) {
            ++mergeCount;
            removed[i] = true;
        } else {
            current = i;
        }
    }
    std::size_t out = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!removed[i]) {
            if (out != i) list[out] = list[i];
            ++out;
        }
    }
    list.resize(out);
    return mergeCount;
}

void LodQuadBuilder::mergeQuads() {
    if (getCurrentOpaqueQuadsCount() + getCurrentTransparentQuadsCount() <= 1) {
        return;
    }
    for (int d = 0; d < 6; ++d) {
        mergeInternal(opaque_, d, MergeDirection::EastWest);
        if (doTransparency_) mergeInternal(transparent_, d, MergeDirection::EastWest);
        if (d == dir::faceIndex(Direction::Up) || d == dir::faceIndex(Direction::Down)) {
            mergeInternal(opaque_, d, MergeDirection::NorthSouthOrUpDown);
            if (doTransparency_) mergeInternal(transparent_, d, MergeDirection::NorthSouthOrUpDown);
        }
    }
}

//========//
// buffer //
//========//

MeshBuffers LodQuadBuilder::build() const {
    MeshBuffers out;
    out.opaque      = makeBuffers(opaque_);
    out.transparent = makeBuffers(transparent_);
    return out;
}

std::vector<std::vector<std::byte>>
LodQuadBuilder::makeBuffers(const std::array<std::vector<BufferQuad>, 6>& lists) const {
    std::vector<std::vector<std::byte>> buffers;
    for (const auto& list : lists) {
        for (const BufferQuad& q : list) {
            if (buffers.empty() || buffers.back().size() + kBytesPerQuad > kMaxBufferByteSize) {
                buffers.emplace_back();
                buffers.back().reserve(kBytesPerQuad * 64);
            }
            putQuad(buffers.back(), q);
        }
    }
    return buffers;
}

namespace {
inline void put8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
inline void put16(std::vector<std::byte>& b, std::uint16_t v) { // little endian (native)
    put8(b, static_cast<std::uint8_t>(v & 0xFF));
    put8(b, static_cast<std::uint8_t>(v >> 8));
}
inline std::int16_t add16(std::int16_t a, int d) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(a + d));
}
} // namespace

void LodQuadBuilder::putQuad(std::vector<std::byte>& buf, const BufferQuad& quad) const {
    const int          face        = dir::faceIndex(quad.direction);
    const auto&        base        = kVertexIbo[face];
    const std::int16_t wEW         = quad.widthEastWest;
    const std::int16_t wNS         = quad.widthNorthSouthOrHeight;
    const std::uint8_t normalIndex = static_cast<std::uint8_t>(face);
    const Axis         axis        = dir::getAxis(quad.direction);
    const bool         grass       = quad.irisBlockMaterialId == static_cast<std::uint8_t>(BlockMaterial::Grass);

    for (int i = 0; i < 4; ++i) {
        int        dx = 0, dy = 0, dz = 0, mx = 0, my = 0, mz = 0;
        const bool a = base[i][0] == 1, b = base[i][1] == 1;
        switch (axis) {
        case Axis::X: // ZY
            dy = b ? wNS : 0;
            dz = a ? wEW : 0;
            my = b ? 1 : -1;
            mz = a ? 1 : -1;
            break;
        case Axis::Y: // XZ
            dx = a ? wEW : 0;
            dz = b ? wNS : 0;
            mx = a ? 1 : -1;
            mz = b ? 1 : -1;
            break;
        case Axis::Z: // XY
            dx = a ? wEW : 0;
            dy = b ? wNS : 0;
            mx = a ? 1 : -1;
            my = b ? 1 : -1;
            break;
        }

        std::uint32_t col = quad.color;
        if (grass && !options_.debugRenderMode && options_.grassSide != GrassSideRendering::AsGrass) {
            const bool horizontal = axis == Axis::X || axis == Axis::Z;
            if (horizontal || quad.direction == Direction::Down) {
                if (options_.grassSide == GrassSideRendering::AsDirt
                    || (options_.grassSide == GrassSideRendering::FadeToDirt && base[i][1] == 0)
                    || quad.direction == Direction::Down) {
                    col =
                        color::applyShade(options_.dirtColorArgb, options_.shadeByFace[static_cast<std::size_t>(face)]);
                }
            }
        }

        if (quad.hasError) {
            putVertex(
                buf,
                add16(quad.x, dx),
                add16(quad.y, dy),
                add16(quad.z, dz),
                color::RED,
                0,
                0,
                15,
                15,
                0,
                mx,
                my,
                mz
            );
        } else {
            putVertex(
                buf,
                add16(quad.x, dx),
                add16(quad.y, dy),
                add16(quad.z, dz),
                col,
                normalIndex,
                quad.irisBlockMaterialId,
                static_cast<std::int8_t>(quad.skyLight),
                static_cast<std::int8_t>(quad.blockLight),
                quad.textureTileId,
                mx,
                my,
                mz
            );
        }
    }
}

void LodQuadBuilder::putVertex(
    std::vector<std::byte>& buf,
    std::int16_t            x,
    std::int16_t            y,
    std::int16_t            z,
    std::uint32_t           col,
    std::uint8_t            normalIndex,
    std::uint8_t            material,
    std::int8_t             sky,
    std::int8_t             block,
    std::uint16_t           tile,
    int                     mx,
    int                     my,
    int                     mz
) const {
    put16(buf, static_cast<std::uint16_t>(x));
    put16(buf, static_cast<std::uint16_t>(y));
    put16(buf, static_cast<std::uint16_t>(z));

    // Java: skylight %= 16 on a (signed) byte; meta |= (short)(sky | (block << 4)).
    const int sk    = sky % 16;
    const int bl    = block % 16;
    int       meta  = static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(sk | (bl << 4))));
    int       micro = 0;
    if (mx != 0) micro |= mx > 0 ? 0b01 : 0b11;
    if (my != 0) micro |= my > 0 ? 0b0100 : 0b1100;
    if (mz != 0) micro |= mz > 0 ? 0b010000 : 0b110000;
    meta |= micro << 8;
    put16(buf, static_cast<std::uint16_t>(meta));

    put8(buf, static_cast<std::uint8_t>(color::getRed(col)));
    put8(buf, static_cast<std::uint8_t>(color::getGreen(col)));
    put8(buf, static_cast<std::uint8_t>(color::getBlue(col)));
    put8(buf, doTransparency_ ? static_cast<std::uint8_t>(color::getAlpha(col)) : 255);
    put8(buf, material);
    put8(buf, normalIndex);
    put16(buf, tile);
}

//=========//
// getters //
//=========//

int LodQuadBuilder::getCurrentOpaqueQuadsCount() const {
    int n = 0;
    for (const auto& l : opaque_) n += static_cast<int>(l.size());
    return n;
}

int LodQuadBuilder::getCurrentTransparentQuadsCount() const {
    if (!doTransparency_) return 0;
    int n = 0;
    for (const auto& l : transparent_) n += static_cast<int>(l.size());
    return n;
}

int LodQuadBuilder::getCurrentNeededVertexBufferCount() const {
    const std::size_t perBuf = kMaxBufferByteSize / kBytesPerQuad;
    auto bufs = [&](int quads) { return static_cast<int>((static_cast<std::size_t>(quads) + perBuf - 1) / perBuf); };
    return bufs(getCurrentOpaqueQuadsCount()) + bufs(getCurrentTransparentQuadsCount());
}

} // namespace dh
