// Tests for WP-F1: BufferQuad + LodQuadBuilder.
// The Java repo has no unit tests referencing LodQuadBuilder / BufferQuad (grep of
// core/src/test/java/tests), so nothing is ported 1:1; the cases below are derived
// from the Java source (BufferQuad.java, LodQuadBuilder.java).
#include <doctest.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dh/mesh/BufferQuad.h"
#include "dh/mesh/LodQuadBuilder.h"
#include "dh/mesh/MeshBuffers.h"

using namespace dh;

namespace {

std::string toHex(const std::vector<std::byte>& b) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    for (std::byte v : b) {
        s.push_back(d[(static_cast<unsigned>(v) >> 4) & 0xF]);
        s.push_back(d[static_cast<unsigned>(v) & 0xF]);
    }
    return s;
}

// strips spaces
std::string compact(std::string s) {
    std::string o;
    for (char c : s)
        if (c != ' ') o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return o;
}

constexpr std::uint32_t kColor = 0xFF112233u; // r=11 g=22 b=33 a=FF

BufferQuad makeQuad(
    Direction     d,
    int           x,
    int           y,
    int           z,
    int           w,
    int           h,
    std::uint32_t c    = kColor,
    int           tile = 0,
    int           mat  = 0,
    int           sky  = 0,
    int           blk  = 0
) {
    BufferQuad q;
    REQUIRE(q.set(
        (int16_t)x,
        (int16_t)y,
        (int16_t)z,
        (int16_t)w,
        (int16_t)h,
        c,
        (uint16_t)tile,
        (uint8_t)mat,
        (uint8_t)sky,
        (uint8_t)blk,
        d
    ));
    return q;
}

} // namespace

//===========//
// BufferQuad //
//===========//

TEST_CASE("BufferQuad: set rejects zero / negative sizes") {
    BufferQuad q;
    CHECK_FALSE(q.set(0, 0, 0, 0, 1, 0, 0, 0, 0, 0, Direction::Up));
    CHECK_FALSE(q.set(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, Direction::Up));
    CHECK_FALSE(q.set(0, 0, 0, -1, 1, 0, 0, 0, 0, 0, Direction::Up));
    CHECK(q.set(0, 0, 0, 1, 1, 0, 0, 0, 0, 0, Direction::Up));
}

TEST_CASE("BufferQuad: sort keys sign-extend shorts as Java does") {
    // Y axis, east-west key = y<<48 | z<<32 | x<<16 with each short sign-extended to 64 bits.
    BufferQuad          q      = makeQuad(Direction::Up, -1, 2, 3, 1, 1);
    const std::uint64_t expect = (2ull << 48) | (3ull << 32) | (0xFFFFFFFFFFFFFFFFull << 16);
    CHECK(static_cast<std::uint64_t>(q.sortKeyEastWest) == expect);
    // north-south key = y<<48 | x<<32 | z<<16
    const std::uint64_t expectNs = (2ull << 48) | (0xFFFFFFFFFFFFFFFFull << 32) | (3ull << 16);
    CHECK(static_cast<std::uint64_t>(q.sortKeyNorthSouth) == expectNs);
}

TEST_CASE("BufferQuad: merge adjacent identical quads (Y axis, both directions)") {
    BufferQuad a = makeQuad(Direction::Up, 0, 5, 0, 4, 4);
    BufferQuad b = makeQuad(Direction::Up, 4, 5, 0, 4, 4);
    CHECK(a.tryMerge(b, MergeDirection::EastWest));
    CHECK(a.widthEastWest == 8);
    CHECK(a.widthNorthSouthOrHeight == 4);

    // Y axis NS merge: perp = z, parallel = y (fixed axis x)
    BufferQuad c = makeQuad(Direction::Up, 0, 5, 0, 4, 4);
    BufferQuad d = makeQuad(Direction::Up, 0, 5, 4, 4, 4);
    CHECK(c.tryMerge(d, MergeDirection::NorthSouthOrUpDown));
    CHECK(c.widthNorthSouthOrHeight == 8);
    CHECK(c.widthEastWest == 4);
}

TEST_CASE("BufferQuad: differing attributes or geometry block merging") {
    auto       base = [] { return makeQuad(Direction::North, 0, 0, 0, 4, 2, kColor, 7, 2, 3, 5); };
    BufferQuad ok   = makeQuad(Direction::North, 4, 0, 0, 4, 2, kColor, 7, 2, 3, 5);
    {
        BufferQuad a = base(), b = ok;
        CHECK(a.tryMerge(b, MergeDirection::EastWest));
        CHECK(a.widthEastWest == 8);
    }
    auto expectNoMerge = [&](BufferQuad b) {
        BufferQuad a = base();
        CHECK_FALSE(a.tryMerge(b, MergeDirection::EastWest));
        CHECK(a.widthEastWest == 4);
    };
    BufferQuad b;
    b        = ok;
    b.color ^= 1;
    expectNoMerge(b);
    b               = ok;
    b.textureTileId = 8;
    expectNoMerge(b);
    b                     = ok;
    b.irisBlockMaterialId = 3;
    expectNoMerge(b);
    b          = ok;
    b.skyLight = 4;
    expectNoMerge(b);
    b            = ok;
    b.blockLight = 6;
    expectNoMerge(b);
    b                         = ok;
    b.widthNorthSouthOrHeight = 3;
    expectNoMerge(b); // parallel width differs
    b   = ok;
    b.y = 1;
    expectNoMerge(b); // fixed axis differs
    b   = ok;
    b.z = 1;
    expectNoMerge(b); // parallel start differs
    b = makeQuad(Direction::North, 5, 0, 0, 4, 2, kColor, 7, 2, 3, 5);
    expectNoMerge(b); // gap
    b           = ok;
    b.direction = Direction::South;
    expectNoMerge(b);
}

TEST_CASE("BufferQuad: maxQuadWidth floorDiv boundary (:277) and width limit") {
    // A ends exactly on the 2048 boundary; B starts across it.
    BufferQuad a = makeQuad(Direction::Up, 2040, 0, 0, 8, 4);
    BufferQuad b = makeQuad(Direction::Up, 2048, 0, 0, 8, 4);
    CHECK_FALSE(a.tryMerge(b, MergeDirection::EastWest));
    // Negative side: floorDiv(-8, 2048) = -1 but floorDiv(0, 2048) = 0 (C++ '/' would say 0 == 0).
    BufferQuad c = makeQuad(Direction::Up, -8, 0, 0, 8, 4);
    BufferQuad d = makeQuad(Direction::Up, 0, 0, 0, 8, 4);
    CHECK_FALSE(c.tryMerge(d, MergeDirection::EastWest));
    // Same block: fine.
    BufferQuad e = makeQuad(Direction::Up, 2032, 0, 0, 8, 4);
    BufferQuad f = makeQuad(Direction::Up, 2040, 0, 0, 8, 4);
    CHECK(e.tryMerge(f, MergeDirection::EastWest));
    // Width >= max blocks merging.
    BufferQuad g = makeQuad(Direction::Up, 0, 0, 0, 2048, 4);
    BufferQuad h = makeQuad(Direction::Up, 2048, 0, 0, 1, 4);
    CHECK_FALSE(g.tryMerge(h, MergeDirection::EastWest));
    BufferQuad i = makeQuad(Direction::Up, 0, 0, 0, 2047, 4);
    BufferQuad j = makeQuad(Direction::Up, 2047, 0, 0, 1, 4);
    CHECK(i.tryMerge(j, MergeDirection::EastWest));
    CHECK(i.widthEastWest == 2048);
}

TEST_CASE("BufferQuad: earth curvature shrinks max width to a chunk") {
    QuadMergeOptions opts;
    opts.earthCurveEnabled = true;
    BufferQuad a           = makeQuad(Direction::Up, 8, 0, 0, 8, 4);
    BufferQuad b           = makeQuad(Direction::Up, 16, 0, 0, 8, 4);
    CHECK_FALSE(a.tryMerge(b, MergeDirection::EastWest, opts)); // floorDiv(16,16) != floorDiv(8,16)
    BufferQuad c = makeQuad(Direction::Up, 0, 0, 0, 8, 4);
    BufferQuad d = makeQuad(Direction::Up, 8, 0, 0, 8, 4);
    CHECK(c.tryMerge(d, MergeDirection::EastWest, opts));
    BufferQuad e = makeQuad(Direction::Up, 0, 0, 0, 16, 4);
    BufferQuad f = makeQuad(Direction::Up, 16, 0, 0, 8, 4);
    CHECK_FALSE(e.tryMerge(f, MergeDirection::EastWest, opts)); // width >= 16
}

TEST_CASE("BufferQuad: merged width wraps as a Java short") {
    BufferQuad a = makeQuad(Direction::Up, 0, 0, 0, 2000, 4);
    BufferQuad b = makeQuad(Direction::Up, 2000, 0, 0, 32767, 4);
    CHECK(a.tryMerge(b, MergeDirection::EastWest));
    CHECK(a.widthEastWest == static_cast<int16_t>(2000 + 32767 - 65536));
}

TEST_CASE("BufferQuad: overlap sets hasError only when enabled") {
    {
        BufferQuad a = makeQuad(Direction::Up, 0, 0, 0, 4, 4), b = makeQuad(Direction::Up, 2, 0, 0, 4, 4);
        CHECK_FALSE(a.tryMerge(b, MergeDirection::EastWest));
        CHECK_FALSE(a.hasError);
        CHECK_FALSE(b.hasError);
    }
    {
        QuadMergeOptions o;
        o.showOverlappingQuadErrors = true;
        BufferQuad a = makeQuad(Direction::Up, 0, 0, 0, 4, 4), b = makeQuad(Direction::Up, 2, 0, 0, 4, 4);
        CHECK_FALSE(a.tryMerge(b, MergeDirection::EastWest, o));
        CHECK(a.hasError);
        CHECK(b.hasError);
        BufferQuad c = makeQuad(Direction::Up, 4, 0, 0, 4, 4);
        CHECK_FALSE(a.tryMerge(c, MergeDirection::EastWest, o)); // errored quads never merge
    }
}

//=================//
// LodQuadBuilder //
//=================//

namespace {

// 16-byte vertex literal: i16 x,y,z | u16 meta (sky|block<<4, micro<<8) | rgba | material | normal | u16 tile
// All six quads below use colour 0xFF112233, tile 0x0102, material 2, sky 3, block 5 -> meta low byte 0x53.
std::string expectedFace(const char* v0, const char* v1, const char* v2, const char* v3) {
    return compact(std::string(v0) + v1 + v2 + v3);
}

MeshBuffers buildOne(Direction d) {
    LodQuadBuilder b;
    switch (d) {
    case Direction::Down:
        b.addQuadDown(10, 20, 30, 4, kColor, 0x0102, 2, 3, 5);
        break;
    case Direction::Up:
        b.addQuadUp(10, 20, 30, 4, kColor, 0x0102, 2, 3, 5);
        break;
    default:
        b.addQuadAdj(d, 10, 20, 30, 4, 2, kColor, 0x0102, 2, 3, 5);
        break;
    }
    return b.build();
}

} // namespace

TEST_CASE("LodQuadBuilder: byte-exact vertices, one quad per face") {
    // x=10(0A) y=20(14) z=30(1E); EW width 4, NS/height 2 (Up/Down: 4x4: 14=0E, z+4=34=22)
    // layout per vertex: x y z (LE i16) | 53 <micro> | 11 22 33 FF | 02 <face> | 02 01
    struct Case {
        Direction   d;
        std::string hex;
    };
    const Case cases[] = {
        {Direction::Down,
         expectedFace(
             "0E00 1400 1E00 5331 112233FF 0200 0201", "0E00 1400 2200 5311 112233FF 0200 0201",
         "0A00 1400 2200 5313 112233FF 0200 0201", "0A00 1400 1E00 5333 112233FF 0200 0201"
         )},
        {Direction::Up,
         expectedFace(
             "0A00 1400 1E00 5333 112233FF 0201 0201", "0A00 1400 2200 5313 112233FF 0201 0201",
         "0E00 1400 2200 5311 112233FF 0201 0201", "0E00 1400 1E00 5331 112233FF 0201 0201"
         )},
        {Direction::North,
         expectedFace(
             "0A00 1400 1E00 530F 112233FF 0202 0201", "0A00 1600 1E00 5307 112233FF 0202 0201",
         "0E00 1600 1E00 5305 112233FF 0202 0201", "0E00 1400 1E00 530D 112233FF 0202 0201"
         )},
        {Direction::South,
         expectedFace(
             "0E00 1400 1E00 530D 112233FF 0203 0201", "0E00 1600 1E00 5305 112233FF 0203 0201",
         "0A00 1600 1E00 5307 112233FF 0203 0201", "0A00 1400 1E00 530F 112233FF 0203 0201"
         )},
        {Direction::West,
         expectedFace(
             "0A00 1400 1E00 533C 112233FF 0204 0201", "0A00 1400 2200 531C 112233FF 0204 0201",
         "0A00 1600 2200 5314 112233FF 0204 0201", "0A00 1600 1E00 5334 112233FF 0204 0201"
         )},
        {Direction::East,
         expectedFace(
             "0A00 1600 1E00 5334 112233FF 0205 0201", "0A00 1600 2200 5314 112233FF 0205 0201",
         "0A00 1400 2200 531C 112233FF 0205 0201", "0A00 1400 1E00 533C 112233FF 0205 0201"
         )},
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.d));
        MeshBuffers m = buildOne(c.d);
        REQUIRE(m.opaque.size() == 1);
        CHECK(m.transparent.empty());
        REQUIRE(m.opaque[0].size() == 64);
        CHECK(toHex(m.opaque[0]) == c.hex);
    }
}

TEST_CASE("LodQuadBuilder: addQuadAdj rejects Down and invalid sizes") {
    LodQuadBuilder b;
    CHECK_FALSE(b.addQuadAdj(Direction::Down, 0, 0, 0, 1, 1, kColor, 0, 0, 0, 0));
    CHECK_FALSE(b.addQuadAdj(Direction::North, 0, 0, 0, 0, 1, kColor, 0, 0, 0, 0));
    CHECK(b.getCurrentQuadsCount() == 0);
}

TEST_CASE("LodQuadBuilder: premerge of consecutive adjacent quads") {
    LodQuadBuilder b;
    b.addQuadAdj(Direction::North, 0, 0, 0, 4, 2, kColor, 0, 0, 0, 0);
    b.addQuadAdj(Direction::North, 4, 0, 0, 4, 2, kColor, 0, 0, 0, 0); // EW first
    CHECK(b.getCurrentOpaqueQuadsCount() == 1);
    CHECK(b.getPremergeCount() == 1);
    CHECK(b.opaqueQuads(Direction::North)[0].widthEastWest == 8);
    // Different light: not merged
    b.addQuadAdj(Direction::North, 8, 0, 0, 4, 2, kColor, 0, 0, 1, 0);
    CHECK(b.getCurrentOpaqueQuadsCount() == 2);
    // Different colour / tile
    b.addQuadAdj(Direction::South, 0, 0, 0, 4, 2, kColor, 0, 0, 0, 0);
    b.addQuadAdj(Direction::South, 4, 0, 0, 4, 2, 0xFF000000u, 0, 0, 0, 0);
    b.addQuadAdj(Direction::South, 8, 0, 0, 4, 2, 0xFF000000u, 9, 0, 0, 0);
    CHECK(b.getCurrentOpaqueQuadsCount() == 5);
    // Vertical (NS/Up-Down) premerge is tried second: stack on y
    LodQuadBuilder v;
    v.addQuadAdj(Direction::East, 0, 0, 0, 4, 2, kColor, 0, 0, 0, 0);
    v.addQuadAdj(Direction::East, 0, 2, 0, 4, 2, kColor, 0, 0, 0, 0);
    // axis X: NS merge perp = y, parallel = z, fixed axis x -> merged height 4
    CHECK(v.getCurrentOpaqueQuadsCount() == 1);
    CHECK(v.opaqueQuads(Direction::East)[0].widthNorthSouthOrHeight == 4);
}

TEST_CASE("LodQuadBuilder: mergeQuads greedy, UP gets a second pass") {
    LodQuadBuilder b;
    // 2x2 grid of 4-wide up quads, inserted shuffled
    b.addQuadUp(4, 10, 4, 4, kColor, 0, 0, 0, 0);
    b.addQuadUp(0, 10, 0, 4, kColor, 0, 0, 0, 0);
    b.addQuadUp(4, 10, 0, 4, kColor, 0, 0, 0, 0);
    b.addQuadUp(0, 10, 4, 4, kColor, 0, 0, 0, 0);
    CHECK(b.getCurrentQuadsCount() == 4);
    b.mergeQuads();
    REQUIRE(b.getCurrentQuadsCount() == 1);
    const BufferQuad& q = b.opaqueQuads(Direction::Up)[0];
    CHECK(q.x == 0);
    CHECK(q.z == 0);
    CHECK(q.widthEastWest == 8);
    CHECK(q.widthNorthSouthOrHeight == 8);
}

TEST_CASE("LodQuadBuilder: mergeQuads on <=1 quads is a no-op") {
    LodQuadBuilder b;
    b.mergeQuads();
    b.addQuadUp(0, 0, 0, 1, kColor, 0, 0, 0, 0);
    b.mergeQuads();
    CHECK(b.getCurrentQuadsCount() == 1);
}

TEST_CASE("LodQuadBuilder: stable sort keeps insertion order of equal keys") {
    // Overlapping (identical position) up quads with different colours have equal sort keys,
    // never merge, and must keep their insertion order (Java TimSort is stable).
    const std::uint32_t colA = 0xFF0000FFu, colB = 0xFF00FF00u, colC = 0xFFFF0000u;
    auto                run = [&](std::vector<std::uint32_t> order) {
        LodQuadBuilder b;
        b.addQuadUp(50, 0, 50, 2, 0xFFFFFFFFu, 0, 0, 0, 0); // sorts after, inserted first
        for (std::uint32_t c : order) b.addQuadUp(0, 0, 0, 2, c, 0, 0, 0, 0);
        b.mergeQuads();
        std::vector<std::uint32_t> got;
        for (const BufferQuad& q : b.opaqueQuads(Direction::Up)) got.push_back(q.color);
        return got;
    };
    CHECK(run({colA, colB, colC}) == std::vector<std::uint32_t>{colA, colB, colC, 0xFFFFFFFFu});
    CHECK(run({colC, colA, colB}) == std::vector<std::uint32_t>{colC, colA, colB, 0xFFFFFFFFu});
    CHECK(run({colB, colC, colA}) == std::vector<std::uint32_t>{colB, colC, colA, 0xFFFFFFFFu});
}

TEST_CASE("LodQuadBuilder: 2 MiB buffer split on quad boundary") {
    static_assert(kMaxBufferByteSize == 2097152);
    static_assert(kMaxBufferByteSize % 64 == 0);
    const int perBuf = static_cast<int>(kMaxBufferByteSize / kBytesPerQuad);
    CHECK(perBuf == 32768);
    auto fill = [](LodQuadBuilder& b, int n) {
        for (int i = 0; i < n; ++i)
            b.addQuadUp(static_cast<int16_t>(i % 200 * 2), 0, static_cast<int16_t>(i / 200 * 2), 1, kColor, 0, 0, 0, 0);
    };
    {
        LodQuadBuilder b;
        fill(b, perBuf);
        CHECK(b.getCurrentNeededVertexBufferCount() == 1);
        MeshBuffers m = b.build();
        REQUIRE(m.opaque.size() == 1);
        CHECK(m.opaque[0].size() == kMaxBufferByteSize);
    }
    {
        LodQuadBuilder b;
        fill(b, perBuf + 1);
        CHECK(b.getCurrentNeededVertexBufferCount() == 2);
        MeshBuffers m = b.build();
        REQUIRE(m.opaque.size() == 2);
        CHECK(m.opaque[0].size() == kMaxBufferByteSize);
        CHECK(m.opaque[1].size() == 64);
    }
    {
        LodQuadBuilder b;
        MeshBuffers    m = b.build();
        CHECK(m.opaque.empty());
        CHECK(m.transparent.empty());
    }
}

TEST_CASE("LodQuadBuilder: transparency routing and alpha byte") {
    const std::uint32_t translucent = 0x80112233u;
    {
        QuadBuilderOptions o;
        o.transparency = Transparency::Complete;
        LodQuadBuilder b(o);
        b.addQuadUp(0, 0, 0, 1, translucent, 0, 0, 0, 0);
        b.addQuadUp(5, 0, 5, 1, kColor, 0, 0, 0, 0);
        CHECK(b.getCurrentOpaqueQuadsCount() == 1);
        CHECK(b.getCurrentTransparentQuadsCount() == 1);
        MeshBuffers m = b.build();
        REQUIRE(m.transparent.size() == 1);
        // alpha byte is vertex byte 11
        CHECK(std::to_integer<int>(m.transparent[0][11]) == 0x80);
    }
    {
        LodQuadBuilder b; // Disabled: translucent colours are opaque, alpha forced to 255
        b.addQuadUp(0, 0, 0, 1, translucent, 0, 0, 0, 0);
        CHECK(b.getCurrentOpaqueQuadsCount() == 1);
        CHECK(b.getCurrentTransparentQuadsCount() == 0);
        MeshBuffers m = b.build();
        REQUIRE(m.opaque.size() == 1);
        CHECK(std::to_integer<int>(m.opaque[0][11]) == 0xFF);
    }
}

TEST_CASE("LodQuadBuilder: hasError quads render red with light 15 and zeroed ids") {
    QuadBuilderOptions o;
    o.showOverlappingQuadErrors = true;
    LodQuadBuilder b(o);
    b.addQuadUp(0, 0, 0, 4, kColor, 0x0102, 2, 3, 5);
    b.addQuadUp(2, 0, 0, 4, kColor, 0x0102, 2, 3, 5);
    b.mergeQuads();
    REQUIRE(b.getCurrentQuadsCount() == 2);
    MeshBuffers m = b.build();
    REQUIRE(m.opaque.size() == 1);
    // first quad (x=0): vertex 0 (UP entry {0,0}) = (0,0,0) micro x-,z- = 0x33
    const std::string hex = toHex(m.opaque[0]);
    CHECK(hex.substr(0, 32) == compact("0000 0000 0000 FF33 FF0000FF 0000 0000"));
}

TEST_CASE("LodQuadBuilder: grass side colouring modes") {
    const std::uint32_t green = 0xFF00FF00u;
    const std::uint32_t dirt  = 0xFF664422u;
    // applyShade(dirt, 0.5) = 33 22 11 (see ColorUtil.applyShade(int,float))
    const std::string dirtBytes = "332211ff", greenBytes = "00ff00ff";
    auto              vertexColor = [](const MeshBuffers& m, int quadVertex) {
        return toHex(m.opaque[0]).substr(quadVertex * 32 + 16, 8);
    };

    auto make = [&](GrassSideRendering mode, bool debug, Direction d) {
        QuadBuilderOptions o;
        o.grassSide       = mode;
        o.debugRenderMode = debug;
        o.dirtColorArgb   = dirt;
        o.shadeByFace.fill(0.5f);
        LodQuadBuilder b(o);
        const uint8_t  grass = 13;
        if (d == Direction::Down) b.addQuadDown(0, 0, 0, 2, green, 0, grass, 0, 0);
        else if (d == Direction::Up) b.addQuadUp(0, 0, 0, 2, green, 0, grass, 0, 0);
        else b.addQuadAdj(d, 0, 0, 0, 2, 2, green, 0, grass, 0, 0);
        return b.build();
    };

    SUBCASE("AS_GRASS leaves colour alone") {
        auto m = make(GrassSideRendering::AsGrass, false, Direction::North);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(m, v) == greenBytes);
    }
    SUBCASE("AS_DIRT tints every vertex of a side face") {
        auto m = make(GrassSideRendering::AsDirt, false, Direction::North);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(m, v) == dirtBytes);
    }
    SUBCASE("FADE_TO_DIRT tints only the bottom two vertices") {
        // North IBO: v0 {0,0}, v1 {0,1}, v2 {1,1}, v3 {1,0}
        auto m = make(GrassSideRendering::FadeToDirt, false, Direction::North);
        CHECK(vertexColor(m, 0) == dirtBytes);
        CHECK(vertexColor(m, 1) == greenBytes);
        CHECK(vertexColor(m, 2) == greenBytes);
        CHECK(vertexColor(m, 3) == dirtBytes);
    }
    SUBCASE("DOWN is always dirt, even FADE_TO_DIRT, but not AS_GRASS") {
        auto m = make(GrassSideRendering::FadeToDirt, false, Direction::Down);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(m, v) == dirtBytes);
        auto g = make(GrassSideRendering::AsGrass, false, Direction::Down);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(g, v) == greenBytes);
    }
    SUBCASE("UP is never tinted") {
        auto m = make(GrassSideRendering::AsDirt, false, Direction::Up);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(m, v) == greenBytes);
    }
    SUBCASE("debug render mode disables the grass logic") {
        auto m = make(GrassSideRendering::AsDirt, true, Direction::North);
        for (int v = 0; v < 4; ++v) CHECK(vertexColor(m, v) == greenBytes);
    }
}
