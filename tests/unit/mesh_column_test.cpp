// Tests for WP-F2: ColumnBox + ColumnRenderBufferBuilder (dh/mesh/ColumnMeshBuilder.h).
// The Java repo has no unit tests for these classes; every expectation below is computed by
// hand from ColumnBox.java / ColumnRenderBufferBuilder.java (line numbers in comments).
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Color.h"
#include "golden_loader.h"

using namespace dh;

namespace {

struct TestLevel final : IClientLevel {
    std::int32_t     minHeight() const override { return -64; }
    std::int32_t     maxHeight() const override { return 320; }
    bool             hasCeiling() const override { return false; }
    bool             hasSkyLight() const override { return true; }
    bool             isEnd() const override { return false; }
    std::string_view dimensionName() const override { return "minecraft:overworld"; }
    std::uint32_t    blockColor(std::int32_t, std::int32_t, std::int32_t, const BiomeInfo*, const BlockInfo*) override {
        return 0xFF000000u;
    }
    std::uint32_t dirtBlockColor() override { return 0xFF8B5A2Bu; }
    float         shade(Direction face) const override {
        switch (face) {
        case Direction::Up:
            return 1.0f;
        case Direction::Down:
            return 0.5f;
        case Direction::North:
        case Direction::South:
            return 0.8f;
        default:
            return 0.6f;
        }
    }
};

constexpr std::uint32_t kColA  = 0xFF336699u;
constexpr std::uint32_t kColB  = 0xFF996633u;
constexpr std::uint32_t kWater = 0x80224488u; // alpha nibble 8 -> alpha() == 0x8F
constexpr int           kStone = static_cast<int>(BlockMaterial::Stone);

ColumnRenderSource mk(int absDetail, int slices) {
    return ColumnRenderSource::createEmpty(sec::encode(static_cast<std::int8_t>(absDetail), 0, 0), slices, 0);
}
void put(ColumnRenderSource& s, int x, int z, int i, std::uint64_t v) {
    s.data[static_cast<std::size_t>(ColumnRenderSource::dataIndex(x, z, s.maxVerticalSlices, i))] = v;
}
std::uint64_t pt(int maxY, int minY, std::uint32_t argb, int sky = 12, int blk = 3, int mat = kStone) {
    return rdp::create(maxY, minY, argb, sky, blk, static_cast<BlockMaterial>(mat));
}

MeshOptions opts() {
    MeshOptions o;
    o.enableCaveCulling = false;
    return o;
}
MeshOptions transparentOpts() {
    MeshOptions o       = opts();
    o.quad.transparency = Transparency::Complete;
    return o;
}

struct Run {
    LodQuadBuilder builder;
    bool           ok;
    explicit Run(const QuadBuilderOptions& qo) : builder(qo), ok(false) {}
};

Run run(const ColumnRenderSource& s, const MeshOptions& o, const AdjacentSources& adj = {}) {
    static TestLevel level;
    Run              r(o.quad);
    r.ok = makeLodRenderData(r.builder, s, level, adj, o);
    return r;
}

std::vector<BufferQuad> quads(const Run& r, Direction d, bool transparent = false) {
    std::vector<BufferQuad> v = transparent ? r.builder.transparentQuads(d) : r.builder.opaqueQuads(d);
    std::stable_sort(v.begin(), v.end(), [](const BufferQuad& a, const BufferQuad& b) { return a.y < b.y; });
    return v;
}
int count(const Run& r, Direction d, bool transparent = false) {
    return static_cast<int>(quads(r, d, transparent).size());
}

std::vector<BufferQuad> atX(std::vector<BufferQuad> v, int x) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const BufferQuad& q) { return q.x != x; }), v.end());
    return v;
}

constexpr int kAdjIdxN = 0, kAdjIdxS = 1, kAdjIdxW = 2, kAdjIdxE = 3;

} // namespace

TEST_CASE("mesh column: compass index convention") {
    CHECK(dir::compassIndex(Direction::North) == kAdjIdxN);
    CHECK(dir::compassIndex(Direction::South) == kAdjIdxS);
    CHECK(dir::compassIndex(Direction::West) == kAdjIdxW);
    CHECK(dir::compassIndex(Direction::East) == kAdjIdxE);
}

TEST_CASE("mesh column: single column in void gets 6 faces, side light 15") {
    auto s = mk(6, 2);
    put(s, 10, 10, 0, pt(80, 60, kColA, 12, 3));
    Run r = run(s, opts());
    CHECK(r.ok);
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 6);
    CHECK(r.builder.getCurrentTransparentQuadsCount() == 0);

    // Up: x=10,y=maxY=80,z=10, w=1, sky = datapoint sky, shade 1.0
    auto up = quads(r, Direction::Up);
    REQUIRE(up.size() == 1);
    CHECK(up[0].x == 10);
    CHECK(up[0].y == 80);
    CHECK(up[0].z == 10);
    CHECK(up[0].widthEastWest == 1);
    CHECK(up[0].widthNorthSouthOrHeight == 1);
    CHECK(up[0].skyLight == 12);
    CHECK(up[0].blockLight == 3);
    CHECK(up[0].color == color::applyShade(kColA, 1.0f));

    // Down: y=minY=60, sky light = 0 (bottom data point does not exist), shade 0.5
    auto dn = quads(r, Direction::Down);
    REQUIRE(dn.size() == 1);
    CHECK(dn[0].y == 60);
    CHECK(dn[0].skyLight == 0);
    CHECK(dn[0].color == color::applyShade(kColA, 0.5f));

    struct Exp {
        Direction d;
        int       x, z;
        float     shade;
    };
    // north/west at the min corner, south at z+1, east at x+1 (ColumnBox.java:153-261)
    const Exp exps[] = {
        {Direction::North, 10, 10, 0.8f},
        {Direction::South, 10, 11, 0.8f},
        {Direction::West,  10, 10, 0.6f},
        {Direction::East,  11, 10, 0.6f},
    };
    for (const Exp& e : exps) {
        auto q = quads(r, e.d);
        REQUIRE(q.size() == 1);
        CHECK(q[0].x == e.x);
        CHECK(q[0].y == 60);
        CHECK(q[0].z == e.z);
        CHECK(q[0].widthNorthSouthOrHeight == 20);
        CHECK(q[0].skyLight == 15);
        CHECK(q[0].blockLight == 3);
        CHECK(q[0].color == color::applyShade(kColA, e.shade));
    }
}

TEST_CASE("mesh column: air never renders") {
    auto s = mk(6, 1);
    put(s, 4, 4, 0, pt(80, 60, kColA, 12, 3, static_cast<int>(BlockMaterial::Air)));
    Run r = run(s, opts());
    CHECK(r.ok);
    CHECK(r.builder.getCurrentQuadsCount() == 0);
}

TEST_CASE("mesh column: two adjacent opaque columns cull the shared face") {
    auto s = mk(6, 1);
    put(s, 10, 10, 0, pt(80, 60, kColA));
    put(s, 11, 10, 0, pt(80, 60, kColB));
    Run r = run(s, opts());
    CHECK(r.ok);
    // 5 faces each; A's east and B's west are covered (-1) and dropped
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 10);
    CHECK(count(r, Direction::East) == 1);
    CHECK(quads(r, Direction::East)[0].x == 12);
    CHECK(count(r, Direction::West) == 1);
    CHECK(quads(r, Direction::West)[0].x == 10);
    CHECK(count(r, Direction::Up) == 2);
    CHECK(count(r, Direction::Down) == 2);
    CHECK(count(r, Direction::North) == 2);
    CHECK(count(r, Direction::South) == 2);
}

TEST_CASE("mesh column: cliff emits only the exposed segment") {
    auto s = mk(6, 1);
    put(s, 10, 10, 0, pt(80, 60, kColA));
    put(s, 11, 10, 0, pt(70, 50, kColB));
    Run r = run(s, opts());
    CHECK(r.ok);
    // A's east at x=11: neighbour covers [50,70) -> [70,80) remains, light 15
    auto e = atX(quads(r, Direction::East), 11);
    REQUIRE(e.size() == 1);
    CHECK(e[0].y == 70);
    CHECK(e[0].widthNorthSouthOrHeight == 10);
    CHECK(e[0].skyLight == 15);
    // B's west at x=11: A covers [60,80) -> [50,60) remains
    auto w = atX(quads(r, Direction::West), 11);
    REQUIRE(w.size() == 1);
    CHECK(w[0].y == 50);
    CHECK(w[0].widthNorthSouthOrHeight == 10);
    CHECK(w[0].skyLight == 15);
}

TEST_CASE("mesh column: overhang segment takes the neighbour's sky light") {
    auto s = mk(6, 2);
    put(s, 10, 10, 0, pt(110, 40, kColA));
    // neighbour: upper [90,100) sky 7, lower [50,60) sky 5
    put(s, 11, 10, 0, pt(100, 90, kColB, 7, 0));
    put(s, 11, 10, 1, pt(60, 50, kColB, 5, 0));
    Run r = run(s, opts());
    CHECK(r.ok);
    // segments: [40,50) 15 | [50,60) covered | [60,90) sky 5 (overhang, min(5,15)) | [90,100) covered | [100,110) 15
    auto e = atX(quads(r, Direction::East), 11);
    REQUIRE(e.size() == 3);
    CHECK(e[0].y == 40);
    CHECK(e[0].widthNorthSouthOrHeight == 10);
    CHECK(e[0].skyLight == 15);
    CHECK(e[1].y == 60);
    CHECK(e[1].widthNorthSouthOrHeight == 30);
    CHECK(e[1].skyLight == 5);
    CHECK(e[2].y == 100);
    CHECK(e[2].widthNorthSouthOrHeight == 10);
    CHECK(e[2].skyLight == 15);
}

TEST_CASE("mesh column: skip rules for top and bottom faces") {
    {
        // stacked, touching: top of lower and bottom of upper are skipped
        auto s = mk(6, 2);
        put(s, 3, 3, 0, pt(80, 70, kColA, 12, 0));
        put(s, 3, 3, 1, pt(70, 60, kColB, 9, 0));
        Run r = run(s, opts());
        CHECK(count(r, Direction::Up) == 1);
        CHECK(quads(r, Direction::Up)[0].y == 80);
        CHECK(count(r, Direction::Down) == 1);
        CHECK(quads(r, Direction::Down)[0].y == 60);
        // lower point's bottom: void below -> sky 0
        CHECK(quads(r, Direction::Down)[0].skyLight == 0);
        CHECK(count(r, Direction::North) == 2);
    }
    {
        // a gap keeps both faces; the upper's bottom takes the lower point's sky light
        auto s = mk(6, 2);
        put(s, 3, 3, 0, pt(80, 70, kColA, 12, 0));
        put(s, 3, 3, 1, pt(60, 50, kColB, 9, 0));
        Run r = run(s, opts());
        CHECK(count(r, Direction::Up) == 2);
        CHECK(count(r, Direction::Down) == 2);
        auto dn = quads(r, Direction::Down);
        CHECK(dn[1].y == 70);
        CHECK(dn[1].skyLight == 9);
    }
}

TEST_CASE("mesh column: transparent water over sand") {
    auto s = mk(6, 2);
    put(s, 10, 10, 0, pt(72, 70, kWater, 14, 0, static_cast<int>(BlockMaterial::Water)));
    put(s, 10, 10, 1, pt(70, 60, kColA, 10, 0, static_cast<int>(BlockMaterial::Sand)));
    Run r = run(s, transparentOpts());
    CHECK(r.ok);
    // water: up + 4 sides in the transparent list; its bottom is skipped (sand directly below, opaque)
    CHECK(r.builder.getCurrentTransparentQuadsCount() == 5);
    CHECK(count(r, Direction::Up, true) == 1);
    CHECK(count(r, Direction::Down, true) == 0);
    // sand: top NOT skipped (water above is transparent), bottom over void, 4 sides -> 6 opaque
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 6);
    CHECK(count(r, Direction::Up) == 1);
    CHECK(quads(r, Direction::Up)[0].y == 70);
    CHECK(count(r, Direction::Down) == 1);
}

TEST_CASE("mesh column: transparent neighbours emit no wall between them") {
    auto s = mk(6, 2);
    for (int x : {10, 11}) {
        put(s, x, 10, 0, pt(72, 70, kWater, 14, 0, static_cast<int>(BlockMaterial::Water)));
        put(s, x, 10, 1, pt(70, 60, kColA, 10, 0, static_cast<int>(BlockMaterial::Sand)));
    }
    Run r = run(s, transparentOpts());
    // water east at x=11 / west at x=11 are covered + transparent input -> dropped
    CHECK(atX(quads(r, Direction::East, true), 11).empty());
    CHECK(atX(quads(r, Direction::West, true), 11).empty());
    CHECK(atX(quads(r, Direction::East, true), 12).size() == 1);
    CHECK(atX(quads(r, Direction::West, true), 10).size() == 1);
}

TEST_CASE("mesh column: void-bottom forces an opaque colour") {
    auto s = mk(6, 1);
    put(s, 5, 5, 0, pt(72, 70, kWater, 14, 0, static_cast<int>(BlockMaterial::Water)));
    Run r = run(s, transparentOpts());
    CHECK(r.ok);
    // nothing below -> alpha forced to 255 -> everything lands in the opaque list
    CHECK(r.builder.getCurrentTransparentQuadsCount() == 0);
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 6);
    CHECK(color::getAlpha(quads(r, Direction::Up)[0].color) == 255);
    CHECK(color::getAlpha(quads(r, Direction::East)[0].color) == 255);
}

TEST_CASE("mesh column: section border uses the neighbour's sky light (same detail, wrapped)") {
    // self column at x=63; east neighbour section column (0,10) is tall and lit with sky 9
    auto self = mk(6, 1);
    put(self, 63, 10, 0, pt(80, 60, kColA, 12, 0));
    auto east = mk(6, 1);
    east.pos  = sec::encode(6, 1, 0);
    put(east, 0, 10, 0, pt(200, 0, kColB, 9, 0));

    AdjacentSources adj;
    adj.bySide[kAdjIdxE]     = &east;
    adj.sameDetail[kAdjIdxE] = true;

    Run r = run(self, opts(), adj);
    CHECK(r.ok);
    // x == blockWidth(1) * 64 -> onBorder; lit; cave culling off -> adjacent sky 9, covered but opaque input
    auto e = atX(quads(r, Direction::East), 64);
    REQUIRE(e.size() == 1);
    CHECK(e[0].y == 60);
    CHECK(e[0].widthNorthSouthOrHeight == 20);
    CHECK(e[0].skyLight == 9);

    // an unlit neighbour (sky 0, block 0) is covered -> no face
    put(east, 0, 10, 0, pt(200, 0, kColB, 0, 0));
    Run r2 = run(self, opts(), adj);
    CHECK(atX(quads(r2, Direction::East), 64).empty());

    // cave culling: border face is only lit when adjMaxY >= caveCullingHeight - minHeight
    put(east, 0, 10, 0, pt(200, 0, kColB, 9, 0));
    MeshOptions o       = opts();
    o.enableCaveCulling = true;
    o.caveCullingHeight = 300; // 300 - (-64) = 364 > 200 -> covered
    Run r3              = run(self, o, adj);
    CHECK(atX(quads(r3, Direction::East), 64).empty());
    o.caveCullingHeight = 100; // 164 <= 200 -> lit
    Run  r4             = run(self, o, adj);
    auto e4             = atX(quads(r4, Direction::East), 64);
    REQUIRE(e4.size() == 1);
    CHECK(e4[0].skyLight == 9);
}

TEST_CASE("mesh column: border test is blockWidth*64 at higher detail, and only on the border") {
    // data detail 1 (absolute 7): blockWidth 2, east face x = 63*2 + 2 = 128 == 2 * 64
    auto self = mk(7, 1);
    put(self, 63, 10, 0, pt(80, 60, kColA, 12, 0));
    auto east = mk(7, 1);
    east.pos  = sec::encode(7, 1, 0);
    put(east, 0, 10, 0, pt(200, 0, kColB, 6, 0));
    AdjacentSources adj;
    adj.bySide[kAdjIdxE] = &east;
    Run  r               = run(self, opts(), adj);
    auto e               = atX(quads(r, Direction::East), 128);
    REQUIRE(e.size() == 1);
    CHECK(e[0].skyLight == 6);
    // Up quad of the same column sits at the section-local x=126, width 2
    auto up = quads(r, Direction::Up);
    REQUIRE(up.size() == 1);
    CHECK(up[0].x == 126);
    CHECK(up[0].widthEastWest == 2);

    // interior neighbour (not on the border) is covered even though lit
    auto in = mk(6, 1);
    put(in, 20, 20, 0, pt(80, 60, kColA, 12, 0));
    put(in, 21, 20, 0, pt(200, 0, kColB, 9, 0));
    Run r2 = run(in, opts());
    CHECK(atX(quads(r2, Direction::East), 21).empty());
}

TEST_CASE("mesh column: coarser neighbour is accepted but not wrapped (face falls back to light 15)") {
    auto self = mk(6, 1);
    put(self, 63, 10, 0, pt(80, 60, kColA, 12, 0));
    auto east = mk(7, 1); // data detail 1 > 0
    east.pos  = sec::encode(7, 1, 0);
    // would be an occluder if it were read
    for (int z = 0; z < 64; ++z) {
        put(east, 0, z, 0, pt(200, 0, kColB, 9, 0));
        put(east, 64 - 1, z, 0, pt(200, 0, kColB, 9, 0));
    }
    AdjacentSources adj;
    adj.bySide[kAdjIdxE] = &east;
    Run r                = run(self, opts(), adj);
    CHECK(r.ok); // no assertion
    // Java: xAdj = 64 is not wrapped, populateColumnView throws, view stays cleared -> full face, light 15
    auto e = atX(quads(r, Direction::East), 64);
    REQUIRE(e.size() == 1);
    CHECK(e[0].y == 60);
    CHECK(e[0].widthNorthSouthOrHeight == 20);
    CHECK(e[0].skyLight == 15);
}

TEST_CASE("mesh column: neighbour finer than self is an assertion failure") {
    auto self = mk(7, 1); // data detail 1
    put(self, 63, 10, 0, pt(80, 60, kColA, 12, 0));
    auto east = mk(6, 1); // data detail 0 < 1
    east.pos  = sec::encode(6, 1, 0);
    put(east, 0, 10, 0, pt(200, 0, kColB, 9, 0));
    AdjacentSources adj;
    adj.bySide[kAdjIdxE] = &east;
    Run r                = run(self, opts(), adj);
    CHECK_FALSE(r.ok);
    // Java catches per column and builds on: the east view is cleared -> full face, light 15
    auto e = atX(quads(r, Direction::East), 128);
    REQUIRE(e.size() == 1);
    CHECK(e[0].skyLight == 15);
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 6);
}

TEST_CASE("mesh column: missing neighbour source gives a full light-15 face") {
    auto self = mk(6, 1);
    put(self, 0, 0, 0, pt(80, 60, kColA, 12, 0));
    Run r = run(self, opts()); // no adjacent sources at all
    CHECK(r.ok);
    CHECK(r.builder.getCurrentOpaqueQuadsCount() == 6);
    CHECK(quads(r, Direction::West)[0].skyLight == 15);
    CHECK(quads(r, Direction::North)[0].skyLight == 15);
}

TEST_CASE("mesh column: empty / negative-height inputs") {
    {
        auto s = mk(6, 0); // no slices: Java populateColumnView throws
        Run  r = run(s, opts());
        CHECK_FALSE(r.ok);
    }
    {
        auto s = mk(6, 1);
        put(s, 1, 1, 0, pt(60, 60, kColA)); // maxY == minY -> blockMaxY == 0 -> skipped. NOTE: min==max==60 not void
        Run r = run(s, opts());
        CHECK(r.ok);
        CHECK(r.builder.getCurrentQuadsCount() == 0);
    }
    {
        auto s = mk(6, 1);
        put(s, 1, 1, 0, pt(50, 60, kColA)); // maxY < minY: IllegalArgumentException
        Run r = run(s, opts());
        CHECK_FALSE(r.ok);
    }
}

TEST_CASE("mesh column: grass side mode reaches the quad builder") {
    auto s = mk(6, 1);
    put(s, 7, 7, 0, pt(80, 60, 0xFF44AA33u, 12, 0, static_cast<int>(BlockMaterial::Grass)));
    static TestLevel level;

    auto build = [&](GrassSideRendering g) {
        MeshOptions o        = opts();
        o.quad.grassSide     = g;
        o.quad.dirtColorArgb = level.dirtBlockColor();
        o.quad.shadeByFace   = {
            level.shade(Direction::Down),
            level.shade(Direction::Up),
            level.shade(Direction::North),
            level.shade(Direction::South),
            level.shade(Direction::West),
            level.shade(Direction::East)
        };
        LodQuadBuilder b(o.quad);
        REQUIRE(makeLodRenderData(b, s, level, {}, o));
        return b.build();
    };
    // single column: opaque buffer holds Down, Up, North, South, West, East quads (64 bytes each)
    auto rgbAt = [](const MeshBuffers& m, int quadIdx) {
        REQUIRE(m.opaque.size() == 1);
        const std::size_t o = static_cast<std::size_t>(quadIdx) * 64 + 8;
        return (static_cast<std::uint32_t>(m.opaque[0][o]) << 16)
             | (static_cast<std::uint32_t>(m.opaque[0][o + 1]) << 8) | static_cast<std::uint32_t>(m.opaque[0][o + 2]);
    };
    const std::uint32_t grassSideShaded = color::applyShade(0xFF44AA33u, 0.6f) & 0xFFFFFFu;
    const std::uint32_t dirtSideShaded  = color::applyShade(0xFF8B5A2Bu, 0.6f) & 0xFFFFFFu;

    MeshBuffers asGrass = build(GrassSideRendering::AsGrass);
    MeshBuffers asDirt  = build(GrassSideRendering::AsDirt);
    CHECK(rgbAt(asGrass, 5) == grassSideShaded); // East
    CHECK(rgbAt(asDirt, 5) == dirtSideShaded);
    CHECK(rgbAt(asDirt, 1) == (0xFF44AA33u & 0xFFFFFFu)); // Up unchanged
}

TEST_CASE("mesh column: brightness / saturation multipliers use the AHSV path only when != 1.0") {
    auto s = mk(6, 1);
    put(s, 2, 2, 0, pt(80, 60, kColA));
    {
        MeshOptions o = opts(); // exactly 1.0f / 1.0f
        Run         r = run(s, o);
        CHECK(quads(r, Direction::Up)[0].color == kColA);
    }
    {
        MeshOptions o          = opts();
        o.brightnessMultiplier = 0.5f;
        Run  r                 = run(s, o);
        auto ahsv              = color::argbToAhsv(kColA);
        auto expect            = color::ahsvToArgb(ahsv[0], ahsv[1], ahsv[2] * 1.0f, ahsv[3] * 0.5f);
        CHECK(expect != kColA);
        CHECK(quads(r, Direction::Up)[0].color == expect);
    }
    {
        MeshOptions o          = opts();
        o.saturationMultiplier = 1.5f;
        Run  r                 = run(s, o);
        auto ahsv              = color::argbToAhsv(kColA);
        auto expect            = color::ahsvToArgb(ahsv[0], ahsv[1], ahsv[2] * 1.5f, ahsv[3] * 1.0f);
        CHECK(quads(r, Direction::Up)[0].color == expect);
    }
}

TEST_CASE("mesh column: debug rendering colours are full bright") {
    auto s = mk(6, 1);
    put(s, 2, 2, 0, pt(80, 60, kColA, 12, 2));
    MeshOptions o    = opts();
    o.debugRendering = DebugRendering::ShowDetail;
    Run  r           = run(s, o);
    auto up          = quads(r, Direction::Up);
    REQUIRE(up.size() == 1);
    CHECK(up[0].color == color::rgbToInt(255, 0, 0)); // detail 0
    CHECK(up[0].blockLight == 15);
    CHECK(up[0].skyLight == 12);
}

TEST_CASE("mesh column: golden mesh suite") {
    DH_GOLDEN_SUITE_OR_SKIP(gv, "mesh");
    // TODO(WP-F2): the `mesh` suite is still being generated. Once tests/golden/mesh.gv.json exists,
    // re-check the case keys against tools/golden-dumper/README.md, build the 3x3 fixture from `in`,
    // run makeLodRenderData and compare build() vertex bytes with the hex strings in `out`.
    MESSAGE("mesh golden suite present (" << gv.cases.size() << " cases) but comparison is not implemented yet");
}
