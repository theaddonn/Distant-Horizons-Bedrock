#include "CloudMesh.h"

#include "dh/mesh/LodQuadBuilder.h"
#include "dh/platform/IVertexSink.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dh::render::cloud {

std::size_t Pattern::count() const {
    std::size_t n = 0;
    for (auto v : solid) n += v != 0;
    return n;
}

Pattern patternFromRgba(std::uint8_t const* rgba, int w, int h) {
    Pattern p;
    if (!rgba || w <= 0 || h <= 0) return p;
    p.w = w;
    p.h = h;
    p.solid.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < p.solid.size(); ++i) p.solid[i] = rgba[i * 4 + 3] > 127 ? 1 : 0;
    return p;
}

Pattern proceduralPattern() {
    // two octaves of hashed value noise on a wrapping lattice, thresholded to ~25% coverage
    Pattern p;
    p.w = p.h = 256;
    p.solid.assign(256u * 256u, 0);
    auto hash = [](std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
        std::uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
        h               = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
    };
    auto octave = [&](int x, int y, int period, std::uint32_t seed) {
        float fx = static_cast<float>(x) / static_cast<float>(period),
              fy = static_cast<float>(y) / static_cast<float>(period);
        int   x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        tx          = tx * tx * (3 - 2 * tx);
        ty          = ty * ty * (3 - 2 * ty);
        const int n = 256 / period;
        auto      v = [&](int a, int b) {
            return hash(static_cast<std::uint32_t>(a % n), static_cast<std::uint32_t>(b % n), seed);
        };
        float a = v(x0, y0) * (1 - tx) + v(x0 + 1, y0) * tx, b = v(x0, y0 + 1) * (1 - tx) + v(x0 + 1, y0 + 1) * tx;
        return a * (1 - ty) + b * ty;
    };
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            float n = octave(x, y, 32, 1) * 0.65f + octave(x, y, 8, 2) * 0.35f;
            p.solid[static_cast<std::size_t>(y) * 256u + static_cast<std::size_t>(x)] = n > 0.58f ? 1 : 0;
        }
    return p;
}

bool findInBrArchive(std::uint8_t const* data, std::size_t n, char const* name, std::size_t& off, std::size_t& len) {
    constexpr std::uint64_t kMagic = 0x267052A0B125277DULL;
    constexpr std::size_t   kEntry = 256, kHeader = 16, kNameMax = 247;
    auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8)
             | (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
    };
    if (!data || !name || n < kHeader) return false;
    const std::uint64_t magic = static_cast<std::uint64_t>(u32(0)) | (static_cast<std::uint64_t>(u32(4)) << 32);
    if (magic != kMagic) return false;
    const std::size_t count = u32(8);
    if (count > (n - kHeader) / kEntry) return false;
    const std::size_t base    = kHeader + count * kEntry;
    const std::size_t nameLen = std::strlen(name);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = kHeader + i * kEntry;
        const std::size_t l = data[e];
        if (l > kNameMax || l != nameLen || std::memcmp(data + e + 1, name, l) != 0) continue;
        const std::size_t o = u32(e + 248), s = u32(e + 252);
        if (o > n - base || s > n - base - o) return false;
        off = base + o;
        len = s;
        return true;
    }
    return false;
}

namespace {

constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;

struct Writer {
    Built*       out[6]; ///< per face index (build(): all six the same Built)
    std::uint8_t r, g, b, a;

    static float shade(int face) {
        switch (face) {
        case kUp:
            return 1.0f;
        case kDown:
            return 0.7f;
        case kNorth:
        case kSouth:
            return 0.9f;
        default:
            return 0.8f;
        }
    }

    static void put16(std::vector<std::byte>& buf, std::int32_t v) {
        const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(v));
        buf.push_back(static_cast<std::byte>(u & 0xFF));
        buf.push_back(static_cast<std::byte>(u >> 8));
    }

    /// (x, y, z) = min corner of the quad (plane coordinate on its own axis), wEW along the first in-plane axis
    /// (X for up/down/north/south, Z for east/west), wNS along the second (Z for up/down, Y for the walls).
    void quad(int face, int x, int y, int z, int wEW, int wNS) {
        if (wEW <= 0 || wNS <= 0) return;
        Built& o = *out[face];
        if (o.buffers.empty() || o.buffers.back().size() + dh::kQuadBytes > dh::kMaxBufferBytes)
            o.buffers.emplace_back();
        auto&       buf = o.buffers.back();
        const float s   = shade(face);
        const auto  cr  = static_cast<std::uint8_t>(std::lround(r * s));
        const auto  cg  = static_cast<std::uint8_t>(std::lround(g * s));
        const auto  cb  = static_cast<std::uint8_t>(std::lround(b * s));
        for (int i = 0; i < 4; ++i) {
            const bool aa = dh::LodQuadBuilder::kVertexIbo[face][i][0] == 1;
            const bool bb = dh::LodQuadBuilder::kVertexIbo[face][i][1] == 1;
            int        dx = 0, dy = 0, dz = 0;
            if (face == kUp || face == kDown) {
                dx = aa ? wEW : 0;
                dz = bb ? wNS : 0;
            } else if (face == kNorth || face == kSouth) {
                dx = aa ? wEW : 0;
                dy = bb ? wNS : 0;
            } else {
                dz = aa ? wEW : 0;
                dy = bb ? wNS : 0;
            }
            put16(buf, x + dx);
            put16(buf, y + dy);
            put16(buf, z + dz);
            put16(buf, 15); // sky 15, block 0, no micro offset
            buf.push_back(static_cast<std::byte>(cr));
            buf.push_back(static_cast<std::byte>(cg));
            buf.push_back(static_cast<std::byte>(cb));
            buf.push_back(static_cast<std::byte>(a));
            buf.push_back(std::byte{0});                 // material
            buf.push_back(static_cast<std::byte>(face)); // normal = face index
            buf.push_back(std::byte{0});                 // tile
            buf.push_back(std::byte{0});
        }
        ++o.quads;
    }
};

/// Core of build()/buildTile(): the cells [i0, i0 + nx) x [j0, j0 + nz) as quads in coordinates relative to
/// (originX, originZ). Neighbours outside the region are read from the pattern, so no artificial walls appear at its
/// edge (and none are missing where a neighbouring region has no cloud).
void buildRegion(
    Pattern const& pat,
    long           i0,
    long           j0,
    long           nx,
    long           nz,
    int            originX,
    int            originZ,
    int            y0,
    int            y1,
    int            c,
    bool           fancy,
    Writer&        w
) {
    std::vector<std::uint8_t> solid(static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz));
    for (long j = 0; j < nz; ++j)
        for (long i = 0; i < nx; ++i) solid[static_cast<std::size_t>(j * nx + i)] = pat.at(i0 + i, j0 + j) ? 1 : 0;
    auto cell = [&](long i, long j) -> bool {
        if (i < 0 || j < 0 || i >= nx || j >= nz) return pat.at(i0 + i, j0 + j);
        return solid[static_cast<std::size_t>(j * nx + i)] != 0;
    };
    auto wx = [&](long i) { return static_cast<int>((i0 + i) * c - originX); };
    auto wz = [&](long j) { return static_cast<int>((j0 + j) * c - originZ); };
    // top / bottom: greedy rectangles
    {
        std::vector<std::uint8_t> used(solid.size(), 0);
        for (long j = 0; j < nz; ++j)
            for (long i = 0; i < nx; ++i) {
                const auto idx = static_cast<std::size_t>(j * nx + i);
                if (!solid[idx] || used[idx]) continue;
                long rw = 1;
                while (i + rw < nx && solid[idx + static_cast<std::size_t>(rw)]
                       && !used[idx + static_cast<std::size_t>(rw)])
                    ++rw;
                long rh = 1;
                for (; j + rh < nz; ++rh) {
                    bool ok = true;
                    for (long k = 0; k < rw && ok; ++k) {
                        const auto q = static_cast<std::size_t>((j + rh) * nx + i + k);
                        ok           = solid[q] && !used[q];
                    }
                    if (!ok) break;
                }
                for (long dj = 0; dj < rh; ++dj)
                    for (long di = 0; di < rw; ++di) used[static_cast<std::size_t>((j + dj) * nx + i + di)] = 1;
                const int qx = wx(i), qz = wz(j), qw = static_cast<int>(rw * c), qh = static_cast<int>(rh * c);
                w.quad(kUp, qx, y1, qz, qw, qh);
                w.quad(kDown, qx, y0, qz, qw, qh);
            }
    }

    if (fancy && y1 > y0) {
        const int th = y1 - y0;
        // north (-Z) / south (+Z) walls: runs along X
        for (long j = 0; j < nz; ++j) {
            for (int side = 0; side < 2; ++side) {
                const long dj = side == 0 ? -1 : 1;
                long       i  = 0;
                while (i < nx) {
                    if (!(solid[static_cast<std::size_t>(j * nx + i)] && !cell(i, j + dj))) {
                        ++i;
                        continue;
                    }
                    long e = i + 1;
                    while (e < nx && solid[static_cast<std::size_t>(j * nx + e)] && !cell(e, j + dj)) ++e;
                    w.quad(
                        side == 0 ? kNorth : kSouth,
                        wx(i),
                        y0,
                        side == 0 ? wz(j) : wz(j + 1),
                        static_cast<int>((e - i) * c),
                        th
                    );
                    i = e;
                }
            }
        }
        // west (-X) / east (+X) walls: runs along Z
        for (long i = 0; i < nx; ++i) {
            for (int side = 0; side < 2; ++side) {
                const long di = side == 0 ? -1 : 1;
                long       j  = 0;
                while (j < nz) {
                    if (!(solid[static_cast<std::size_t>(j * nx + i)] && !cell(i + di, j))) {
                        ++j;
                        continue;
                    }
                    long e = j + 1;
                    while (e < nz && solid[static_cast<std::size_t>(e * nx + i)] && !cell(i + di, e)) ++e;
                    w.quad(
                        side == 0 ? kWest : kEast,
                        side == 0 ? wx(i) : wx(i + 1),
                        y0,
                        wz(j),
                        static_cast<int>((e - j) * c),
                        th
                    );
                    j = e;
                }
            }
        }
    }
}

} // namespace


Built build(Pattern const& pat, BuildParams const& p) {
    Built out;
    if (pat.empty() || p.cell <= 0) return out;
    const double half = std::min(p.halfExtent, 30000.0);
    const long   i0   = static_cast<long>(std::floor((p.centerX - half) / p.cell)),
               i1     = static_cast<long>(std::ceil((p.centerX + half) / p.cell)),
               j0     = static_cast<long>(std::floor((p.centerZ - half) / p.cell)),
               j1     = static_cast<long>(std::ceil((p.centerZ + half) / p.cell));
    const long nx = i1 - i0, nz = j1 - j0;
    if (nx <= 0 || nz <= 0) return out;
    Writer    w{{&out, &out, &out, &out, &out, &out}, p.r, p.g, p.b, p.a};
    const int y0 = static_cast<int>(std::lround(p.yBottom));
    const int y1 = p.fancy ? static_cast<int>(std::lround(p.yBottom + p.thickness)) : y0;
    buildRegion(pat, i0, j0, nx, nz, p.anchorX, p.anchorZ, y0, y1, p.cell, p.fancy, w);
    return out;
}

std::array<Built, 6> buildTile(Pattern const& pat, TileParams const& t) {
    std::array<Built, 6> out;
    if (pat.empty() || t.cell <= 0) return out;
    Writer    w{{&out[0], &out[1], &out[2], &out[3], &out[4], &out[5]}, t.r, t.g, t.b, t.a};
    const int y0 = static_cast<int>(std::lround(t.yBottom));
    const int y1 = t.fancy ? static_cast<int>(std::lround(t.yBottom + t.thickness)) : y0;
    buildRegion(
        pat,
        static_cast<long>(t.ti) * kTileCells,
        static_cast<long>(t.tj) * kTileCells,
        kTileCells,
        kTileCells,
        t.ti * kTileCells * t.cell,
        t.tj * kTileCells * t.cell,
        y0,
        y1,
        t.cell,
        t.fancy,
        w
    );
    return out;
}

double rectDistance(double x0, double z0, double x1, double z1, double px, double pz) {
    const double dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0);
    const double dz = pz < z0 ? z0 - pz : (pz > z1 ? pz - z1 : 0.0);
    return std::sqrt(dx * dx + dz * dz);
}

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
) {
    // Mirrors dhCloudBackFace (dot(n, P - camera) >= 0 drops the face) for the extreme plane of each group: every
    // face of a group lies in [min, max] on its normal axis, so the group is dropped entirely only when the camera is
    // on the back side of the whole range.
    unsigned m = 0;
    if (camY >= slabBottom) m |= 1u << kDown;  // down faces at y0 <= slabBottom <= camY
    if (camY <= slabTop) m |= 1u << kUp;       // up faces at y1 = slabTop >= camY
    if (camZ >= maxZ) m |= 1u << kNorth;       // normal -Z: z <= camZ for every wall
    if (camZ <= minZ) m |= 1u << kSouth;       // normal +Z
    if (camX >= maxX) m |= 1u << kWest;        // normal -X
    if (camX <= minX) m |= 1u << kEast;        // normal +X
    return m;
}

} // namespace dh::render::cloud
