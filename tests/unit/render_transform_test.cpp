// Tests for WP-E2: FullDataToRenderDataTransformer (render/FullToRender).
// Hand-built FullDataSources; expectations derived from the Java source
// (FullDataToRenderDataTransformer.java) line by line.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/testing/TestClientLevel.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/FullToRender.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Color.h"
#include "golden_loader.h"

using namespace dh;

namespace {

struct Pt {
    const BlockInfo* block;
    int              bottom;
    int              height;
    int              sky = 15;
    int              blk = 0;
};

struct Fixture {
    MapBlockRegistry reg;
    TestClientLevel  level;
    TransformOptions opt;
    FullDataSource   src;
    const BiomeInfo* biome;

    const BlockInfo *stone, *grass, *poppy, *ice, *glass, *glassB, *lava, *snow2, *snow1, *kelp, *lily, *sand, *barrier;

    explicit Fixture(std::uint64_t pos = sec::encode(6, 0, 0)) {
        level.minY  = 0;
        biome       = reg.defaultBiome();
        src         = FullDataSource::createEmpty(pos);
        src.isEmpty = false;

        stone = add("minecraft:stone", BlockMaterial::Stone);
        grass = add("minecraft:grass_block", BlockMaterial::Grass);
        sand  = add("minecraft:sand", BlockMaterial::Sand);
        BlockInfo b;
        b.serial   = "minecraft:poppy";
        b.isSolid  = false;
        b.opacity  = 0;
        b.material = BlockMaterial::Leaves;
        poppy      = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:ice";
        b.isIce    = true;
        b.opacity  = 3;
        ice        = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:glass";
        b.opacity  = 1;
        b.material = BlockMaterial::Metal;
        glass      = reg.addBlock(b);
        b.serial   = "minecraft:stained_glass";
        glassB     = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:lava";
        b.isLiquid = true;
        b.isSolid  = false;
        b.opacity  = 16;
        b.material = BlockMaterial::Lava;
        lava       = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:snow_STATE_{layers:2}";
        b.isSolid  = true;
        b.opacity  = 16;
        b.material = BlockMaterial::Snow;
        snow2      = reg.addBlock(b);
        b.serial   = "minecraft:snow_STATE_{layers:1}";
        b.isSolid  = false;
        b.opacity  = 0;
        snow1      = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:kelp";
        b.isSolid  = false;
        b.opacity  = 0;
        b.material = BlockMaterial::Leaves;
        kelp       = reg.addBlock(b);
        b.serial   = "minecraft:lily_pad";
        lily       = reg.addBlock(b);
        b          = {};
        b.serial   = "minecraft:barrier";
        barrier    = reg.addBlock(b);

        // air / flowers / snow / kelp: realistic colours -> air has alpha 0
        level.colorOverride[reg.air()]   = 0x00000000u;
        level.colorOverride[poppy]       = 0xFFFF0000u;
        level.colorOverride[snow2]       = 0x80F0F0F0u;
        level.colorOverride[snow1]       = 0x80F0F0F0u;
        level.colorOverride[ice]         = 0xC0A0A0FFu;
        level.colorOverride[glass]       = 0x80C0C0C0u;
        level.colorOverride[glassB]      = 0x80FF8080u;
        level.colorOverride[reg.water()] = 0x800000FFu;
        level.colorOverride[kelp]        = 0xFF008000u;
        level.colorOverride[lily]        = 0xFF00FF00u;

        opt.enableCaveCulling = false;
    }

    const BlockInfo* add(const char* serial, BlockMaterial m) {
        BlockInfo b;
        b.serial   = serial;
        b.material = m;
        return reg.addBlock(b);
    }

    void setColumn(int x, int z, const std::vector<Pt>& pts) {
        std::vector<std::uint64_t> col;
        for (const Pt& p : pts) {
            const int id = src.mapping.addIfAbsent(biome, p.block);
            col.push_back(
                fdp::encode(id, p.height, p.bottom, static_cast<std::uint8_t>(p.blk), static_cast<std::uint8_t>(p.sky))
            );
        }
        src.columns[static_cast<std::size_t>(FullDataSource::relativePosToIndex(x, z))] = std::move(col);
    }

    ColumnRenderSource run() {
        auto r = transformFullDataToRenderSource(src, level, reg, opt);
        REQUIRE(r.has_value());
        return std::move(*r);
    }
};

struct Out {
    int           top, bottom, sky, blk;
    std::uint32_t argb;
    BlockMaterial mat;
};
Out dp(const ColumnRenderSource& s, int i, int x = 0, int z = 0) {
    const std::uint64_t d = s.dataPoint(x, z, i);
    return {rdp::yMax(d), rdp::yMin(d), rdp::skyLight(d), rdp::blockLight(d), rdp::argb(d), rdp::material(d)};
}
// colour after the 4-bit alpha quantisation of the render data point
std::uint32_t q(std::uint32_t argb) { return rdp::argb(rdp::create(10, 0, argb, 0, 0, BlockMaterial::Unknown)); }
int           count(const ColumnRenderSource& s, int x = 0, int z = 0) {
    int n = 0;
    for (int i = 0; i < s.maxVerticalSlices; i++) {
        if (s.dataPoint(x, z, i) != rdp::EMPTY_DATA) n++;
    }
    return n;
}

struct CaptureLogger final : ILogger {
    int  warns = 0;
    void log(LogLevel l, std::string_view) override {
        if (l == LogLevel::Warn) warns++;
    }
};

} // namespace

TEST_CASE("snow layer serial detection") {
    CHECK(isSnowLayerSerial("minecraft:snow_STATE_{layers:1}"));
    CHECK(isSnowLayerSerial("minecraft:snow_STATE_{layers:3}"));
    CHECK_FALSE(isSnowLayerSerial("minecraft:snow_STATE_{layers:4}"));
    CHECK_FALSE(isSnowLayerSerial("minecraft:snow"));
    CHECK(isSnowLayerSerial("minecraft:snow[layers=2]"));
    CHECK(isSnowLayerSerial("minecraft:snow_layer_STATE_{layers:2}"));
    CHECK(isSnowLayerSerial("minecraft:snow_layer[height=0]"));
    CHECK(isSnowLayerSerial("minecraft:snow_layer[height=2]"));
    CHECK_FALSE(isSnowLayerSerial("minecraft:snow_layer[height=3]"));
    CHECK_FALSE(isSnowLayerSerial("minecraft:stone[layers=1]"));
}

TEST_CASE("resolveCsv") {
    MapBlockRegistry reg;
    reg.addBlock(BlockInfo{.serial = "minecraft:a"});
    reg.addBlock(BlockInfo{.serial = "minecraft:b"});
    const BlockSet s = resolveCsv(" minecraft:a, minecraft:b ,,minecraft:nope", reg);
    CHECK(s.size() == 2);
    CHECK(s.count(reg.resolveBlock("minecraft:a")) == 1);
    CHECK(resolveCsv("", reg).empty());
}

TEST_CASE("empty source and sampling position") {
    Fixture f;
    f.src.isEmpty = true;
    auto r        = transformFullDataToRenderSource(f.src, f.level, f.reg, f.opt);
    REQUIRE(r.has_value());
    CHECK(r->isEmpty);

    struct Rec : TestClientLevel {
        int           x = 0, y = 0, z = 0;
        std::uint32_t blockColor(
            std::int32_t     bx,
            std::int32_t     by,
            std::int32_t     bz,
            const BiomeInfo* b,
            const BlockInfo* bl
        ) override {
            x = bx;
            y = by;
            z = bz;
            return TestClientLevel::blockColor(bx, by, bz, b, bl);
        }
    } rec;
    Fixture g(sec::encode(6, 1, 2));
    rec.minY = -64;
    g.setColumn(
        3,
        4,
        {
            {g.stone, 10, 5}
    }
    );
    auto out = transformFullDataToRenderSource(g.src, rec, g.reg, g.opt);
    REQUIRE(out.has_value());
    CHECK_FALSE(out->isEmpty);
    CHECK(rec.x == 64 + 3);
    CHECK(rec.z == 128 + 4);
    CHECK(rec.y == 15 - 1 - 64);
    CHECK(out->yOffset == -64);
}

TEST_CASE("snow layers") {
    Fixture f;
    // 1 layer of snow (height 1) over grass over stone: snow vanishes and tints the grass
    f.setColumn(
        0,
        0,
        {
            {f.snow1, 11, 1,  15, 3},
            {f.grass, 10, 1,  0,  0},
            {f.stone, 0,  10, 0,  0}
    }
    );
    // 2 layer snow, height 2 -> blockHeight 1, snow's own colour (no forced alpha)
    f.setColumn(
        1,
        0,
        {
            {f.snow2, 10, 2,  15, 0},
            {f.stone, 0,  10, 0,  0}
    }
    );
    auto out = f.run();

    Out a = dp(out, 0);
    CHECK(a.top == 11);
    CHECK(a.bottom == 10);
    CHECK(a.argb == 0xFFF0F0F0u);        // colour carried with alpha forced to 255
    CHECK(a.mat == BlockMaterial::Snow); // block carried too (tintWithAvoidedBlocks path)
    CHECK(a.sky == 15);                  // light carried from the snow
    CHECK(a.blk == 3);
    Out b = dp(out, 1);
    CHECK(b.top == 10);
    CHECK(b.bottom == 0);
    CHECK(b.argb == f.level.defaultColor(f.biome, f.stone));
    CHECK(count(out) == 2);

    Out c = dp(out, 0, 1, 0);
    CHECK(c.top == 11); // blockHeight 2 -> 1
    CHECK(c.bottom == 10);
    CHECK(c.argb == q(0x80F0F0F0u));
    CHECK(c.mat == BlockMaterial::Snow);
}

TEST_CASE("flowers (non solid avoided blocks)") {
    for (int tint = 0; tint < 2; tint++) {
        Fixture f;
        f.opt.tintWithAvoidedBlocks = tint == 1;
        f.setColumn(
            0,
            0,
            {
                {f.poppy, 11, 1,  14, 6},
                {f.grass, 0,  11, 5,  0}
        }
        );
        auto o = f.run();
        REQUIRE(count(o) == 1);
        Out a = dp(o, 0);
        CHECK(a.sky == 14); // lights always carried
        CHECK(a.blk == 6);
        if (tint) {
            CHECK(a.argb == 0xFFFF0000u);
            CHECK(a.mat == BlockMaterial::Leaves);
        } else {
            CHECK(a.argb == f.level.defaultColor(f.biome, f.grass));
            CHECK(a.mat == BlockMaterial::Grass);
        }
        CHECK(a.top == 11);
        CHECK(a.bottom == 0);
    }
    // alpha 0 flower transfers nothing
    Fixture f;
    f.level.colorOverride[f.poppy] = 0x00FF0000u;
    f.setColumn(
        0,
        0,
        {
            {f.poppy, 11, 1,  14, 6},
            {f.grass, 0,  11, 5,  1}
    }
    );
    auto o = f.run();
    Out  a = dp(o, 0);
    CHECK(a.sky == 5);
    CHECK(a.blk == 1);
    CHECK(a.argb == f.level.defaultColor(f.biome, f.grass));
    // blocksToAvoid None: the flower is its own datapoint
    Fixture g;
    g.opt.blocksToAvoid = BlocksToAvoid::None;
    g.setColumn(
        0,
        0,
        {
            {g.poppy, 11, 1,  14, 6},
            {g.grass, 0,  11, 5,  0}
    }
    );
    auto og = g.run();
    CHECK(count(og) == 2);
    CHECK(dp(og, 0).argb == 0xFFFF0000u);
}

TEST_CASE("air resets carried state") {
    Fixture f;
    f.setColumn(
        0,
        0,
        {
            {f.poppy,     21, 1,  14, 0},
            {f.reg.air(), 20, 1,  15, 0},
            {f.stone,     0,  20, 3,  0}
    }
    );
    f.setColumn(
        1,
        0,
        {
            {f.poppy, 21, 1,  14, 0},
            {f.stone, 0,  21, 3,  0}
    }
    );
    auto o = f.run();
    Out  a = dp(o, 0);
    CHECK(a.argb == f.level.defaultColor(f.biome, f.stone));
    CHECK(a.sky == 3);
    // control: without the air gap the stone takes the poppy colour
    CHECK(dp(o, 0, 1, 0).argb == 0xFFFF0000u);
}

TEST_CASE("water under ice takes ice and merges") {
    Fixture f;
    f.setColumn(
        0,
        0,
        {
            {f.ice,         20, 1 },
            {f.reg.water(), 17, 3 },
            {f.stone,       0,  17}
    }
    );
    auto o = f.run();
    REQUIRE(count(o) == 2);
    Out a = dp(o, 0);
    CHECK(a.top == 21);
    CHECK(a.bottom == 17);
    CHECK(a.mat != BlockMaterial::Water);
    CHECK(a.argb == q(0xC0A0A0FFu));
    // water without ice stays water
    Fixture g;
    g.setColumn(
        0,
        0,
        {
            {g.reg.water(), 17, 3 },
            {g.stone,       0,  17}
    }
    );
    auto og = g.run();
    CHECK(dp(og, 0).mat == BlockMaterial::Water);
}

TEST_CASE("water replacement sets") {
    Fixture        f;
    const BlockSet sub           = resolveCsv("minecraft:kelp", f.reg);
    const BlockSet surf          = resolveCsv("minecraft:lily_pad", f.reg);
    f.opt.waterSubSurfaceReplace = &sub;
    f.opt.waterSurfaceReplace    = &surf;

    // kelp with air above becomes water (liquid -> rendered as a datapoint), stone below
    f.setColumn(
        0,
        0,
        {
            {f.reg.air(), 12, 1},
            {f.kelp,      8,  4},
            {f.sand,      0,  8}
    }
    );
    // kelp with water above stays kelp (non-solid, carried onto the sand)
    f.setColumn(
        1,
        0,
        {
            {f.reg.water(), 12, 1},
            {f.kelp,        8,  4},
            {f.sand,        0,  8}
    }
    );
    // kelp at the very top of the column (lastBlock null) also becomes water
    f.setColumn(
        2,
        0,
        {
            {f.kelp, 8, 4},
            {f.sand, 0, 8}
    }
    );
    // lily pad of height 2 -> water, height 1
    f.setColumn(
        3,
        0,
        {
            {f.lily, 8, 2},
            {f.sand, 0, 8}
    }
    );
    // lily pad of height 1 vanishes and tints the block below with the water colour (alpha forced by tint)
    f.setColumn(
        4,
        0,
        {
            {f.lily, 8, 1},
            {f.sand, 0, 8}
    }
    );
    auto o = f.run();

    Out a = dp(o, 0, 0, 0);
    CHECK(a.mat == BlockMaterial::Water);
    CHECK(a.top == 12);
    CHECK(a.bottom == 8);
    CHECK(a.argb == q(0x800000FFu));

    Out b = dp(o, 0, 1, 0);
    // column 1: water point, then sand carrying the kelp colour and material
    CHECK(b.mat == BlockMaterial::Water);
    CHECK(dp(o, 1, 1, 0).argb == 0xFF008000u);
    CHECK(dp(o, 1, 1, 0).mat == BlockMaterial::Leaves);

    CHECK(dp(o, 0, 2, 0).mat == BlockMaterial::Water);

    // Java quirk: the lily pad is non-solid, so even at height 1 it is skipped and tints the block below
    Out d = dp(o, 0, 3, 0);
    CHECK(d.mat == BlockMaterial::Water);
    CHECK(d.top == 8);
    CHECK(d.bottom == 0);
    CHECK(d.argb == 0xFF0000FFu);

    Out e = dp(o, 0, 4, 0);
    CHECK(e.bottom == 0);
    CHECK(e.top == 8);
    CHECK(e.argb == (q(0x800000FFu) | 0xFF000000u));
    CHECK(e.mat == BlockMaterial::Water);
}

TEST_CASE("cave culling") {
    auto build = [](Fixture& f, int airSky, int nextSky) {
        f.opt.enableCaveCulling = true;
        f.opt.caveCullingHeight = 100;
        f.setColumn(
            0,
            0,
            {
                {f.stone,     60, 10, 15,      0},
                {f.reg.air(), 30, 30, airSky,  0},
                {f.stone,     0,  30, nextSky, 0}
        }
        );
    };
    {
        Fixture f;
        build(f, 0, 0);
        auto o = f.run();
        REQUIRE(count(o) == 2);
        CHECK(dp(o, 0).top == 70);
        CHECK(dp(o, 0).bottom == 30); // previous point extended over the culled cave
        CHECK(dp(o, 1).top == 30);
        CHECK(dp(o, 1).bottom == 0);
    }
    {
        Fixture f; // air has skylight: not a cave
        build(f, 15, 0);
        auto o = f.run();
        CHECK(dp(o, 0).bottom == 60);
    }
    {
        Fixture f; // next point is lit: culled but not extended
        build(f, 0, 7);
        auto o = f.run();
        CHECK(count(o) == 2);
        CHECK(dp(o, 0).bottom == 60);
    }
    {
        Fixture f; // disabled
        build(f, 0, 0);
        f.opt.enableCaveCulling = false;
        CHECK(dp(f.run(), 0).bottom == 60);
    }
    {
        Fixture f; // ceiling dimension
        build(f, 0, 0);
        f.level.ceiling = true;
        CHECK(dp(f.run(), 0).bottom == 60);
    }
    {
        Fixture f; // the end
        build(f, 0, 0);
        f.level.end = true;
        CHECK(dp(f.run(), 0).bottom == 60);
    }
    {
        Fixture f; // above the culling height
        build(f, 0, 0);
        f.opt.caveCullingHeight = 60;
        CHECK(dp(f.run(), 0).bottom == 60);
    }
    {
        Fixture f; // translucent previous point: culled, but not extended
        f.opt.enableCaveCulling = true;
        f.opt.caveCullingHeight = 100;
        f.setColumn(
            0,
            0,
            {
                {f.glass,     60, 10, 15, 0},
                {f.reg.air(), 30, 30, 0,  0},
                {f.stone,     0,  30, 0,  0}
        }
        );
        auto o = f.run();
        CHECK(dp(o, 0).bottom == 60);
        CHECK(count(o) == 2);
    }
    {
        Fixture f; // top of the column is never culled (fullDataIndex == 0)
        f.opt.enableCaveCulling = true;
        f.opt.caveCullingHeight = 100;
        f.setColumn(
            0,
            0,
            {
                {f.glass, 60, 10, 0, 0},
                {f.stone, 0,  60, 0, 0}
        }
        );
        CHECK(count(f.run()) == 2);
    }
}

TEST_CASE("floating island over void: setVoidAir") {
    Fixture f;
    f.setColumn(
        0,
        0,
        {
            {f.stone,     50, 10, 15, 0},
            {f.reg.air(), 0,  50, 7,  3}
    }
    );
    // only air: the column is void, void datapoint spans the air
    f.setColumn(
        1,
        0,
        {
            {f.reg.air(), 0, 50, 9, 1}
    }
    );
    auto o = f.run();
    CHECK(count(o) == 1);
    const std::uint64_t v = o.voidData[static_cast<std::size_t>(ColumnRenderSource::voidIndex(0, 0))];
    CHECK(rdp::yMax(v) == 50); // previous datapoint's bottom
    CHECK(rdp::yMin(v) == 0);
    CHECK(rdp::skyLight(v) == 7);
    CHECK(rdp::blockLight(v) == 3);
    CHECK(rdp::material(v) == BlockMaterial::Air);
    CHECK(rdp::alpha(v) == 15); // INVISIBLE after quantisation

    CHECK(o.dataPoint(1, 0, 0) == rdp::EMPTY_DATA);
    const std::uint64_t v1 = o.voidData[static_cast<std::size_t>(ColumnRenderSource::voidIndex(1, 0))];
    CHECK(rdp::yMax(v1) == 50);
    CHECK(rdp::skyLight(v1) == 9);
}

TEST_CASE("ignored blocks and void columns") {
    Fixture        f;
    const BlockSet ign  = resolveCsv("minecraft:barrier", f.reg);
    f.opt.ignoredBlocks = &ign;
    // solid ignored block: sky light carried onto the next datapoint
    f.setColumn(
        0,
        0,
        {
            {f.barrier, 11, 1,  9, 0},
            {f.stone,   0,  11, 2, 0}
    }
    );
    // only an ignored block -> void column
    f.setColumn(
        1,
        0,
        {
            {f.barrier, 0, 5, 9, 0}
    }
    );
    auto o = f.run();
    CHECK(count(o) == 1);
    CHECK(dp(o, 0).sky == 9);
    CHECK(dp(o, 0).top == 11);
    CHECK(o.dataPoint(1, 0, 0) == rdp::EMPTY_DATA);

    // ignored cave block (non solid) as bottom datapoint writes the void data
    Fixture        g;
    const BlockSet cave = resolveCsv("minecraft:stone", g.reg);
    g.opt.ignoredBlocks = &cave;
    g.setColumn(
        0,
        0,
        {
            {g.sand,  5, 5, 15, 0},
            {g.stone, 0, 5, 4,  2}
    }
    );
    auto og = g.run();
    CHECK(count(og) == 1);
}

TEST_CASE("lava and translucent glass") {
    Fixture f;
    f.setColumn(
        0,
        0,
        {
            {f.lava, 10, 3, 15, 15},
            {f.stone, 0, 10}
    }
    );
    f.setColumn(
        1,
        0,
        {
            {f.glass,  12, 1 },
            {f.glass,  11, 1 },
            {f.glassB, 10, 1 },
            {f.stone,  0,  10}
    }
    );
    auto o = f.run();
    Out  l = dp(o, 0, 0, 0);
    CHECK(l.mat == BlockMaterial::Lava);
    CHECK(l.top == 13);
    CHECK(l.bottom == 10);
    CHECK(l.blk == 15);
    REQUIRE(count(o, 1, 0) == 3);
    CHECK(dp(o, 0, 1, 0).top == 13);
    CHECK(dp(o, 0, 1, 0).bottom == 11); // same colour merged
    CHECK(dp(o, 1, 1, 0).argb == q(0x80FF8080u));
}

TEST_CASE("same colour merge") {
    Fixture f;
    f.setColumn(
        0,
        0,
        {
            {f.stone, 10, 2},
            {f.stone, 8,  2},
            {f.stone, 0,  8}
    }
    ); // adjacent: all merge
    f.setColumn(
        1,
        0,
        {
            {f.stone, 10, 2},
            {f.stone, 5,  2}
    }
    ); // gap: no merge
    auto o = f.run();
    REQUIRE(count(o) == 1);
    CHECK(dp(o, 0).top == 12);
    CHECK(dp(o, 0).bottom == 0);
    CHECK(count(o, 1, 0) == 2);
}

TEST_CASE("broken id is skipped, warned once per position") {
    CaptureLogger cap;
    ILogger*      prev = log::set(&cap);
    Fixture       f(sec::encode(6, 5, 5));
    f.src.mapping.sectionPos = 0xBAD00001ull;
    f.setColumn(
        0,
        0,
        {
            {f.stone, 5, 5}
    }
    );
    const int           ok  = f.src.mapping.addIfAbsent(f.biome, f.stone);
    const std::uint64_t bad = fdp::encode(ok + 40, 3, 20, 0, 15);
    f.src.columns[static_cast<std::size_t>(FullDataSource::relativePosToIndex(0, 0))].insert(
        f.src.columns[static_cast<std::size_t>(FullDataSource::relativePosToIndex(0, 0))].begin(),
        bad
    );
    f.src.columns[static_cast<std::size_t>(FullDataSource::relativePosToIndex(1, 0))] = {bad};
    auto o                                                                            = f.run();
    log::set(prev);
    CHECK(cap.warns == 1);
    CHECK(count(o, 0, 0) == 1);
    CHECK(dp(o, 0).top == 10);
    CHECK(o.dataPoint(1, 0, 0) == rdp::EMPTY_DATA);
}

TEST_CASE("long column is reduced to maxVerticalSlices") {
    Fixture         f;
    std::vector<Pt> pts;
    for (int i = 0; i < 20; i++) pts.push_back({(i % 2) ? f.stone : f.sand, 19 - i, 1});
    // leave gaps so datapoints are not adjacent after the colours differ
    f.setColumn(0, 0, pts);
    f.opt.verticalQuality = VerticalQuality::Medium; // 6 slices at detail 0
    auto o                = f.run();
    CHECK(o.maxVerticalSlices == 6);
    CHECK(count(o) <= 6);
    CHECK(count(o) >= 1);
    CHECK(dp(o, 0).top == 20);

    Fixture g;
    g.setColumn(
        0,
        0,
        pts.empty() ? pts
                    : std::vector<Pt>{
                          {g.stone, 19, 1},
                          {g.sand,  18, 1},
                          {g.stone, 17, 1}
    }
    );
    g.opt.verticalQuality = VerticalQuality::HeightMap;
    auto h                = g.run();
    CHECK(h.maxVerticalSlices == 1);
    CHECK(count(h) == 1);
    CHECK(dp(h, 0).top == 20);
}

TEST_CASE("golden f2r") {
    DH_GOLDEN_SUITE_OR_SKIP(suite, "f2r");
    // TODO(E2): the golden file is being generated; the shape below is a guess
    // (in.source{pos,mapping[{block,biome}],columns[{x,z,data[hex]}]}, in.options, out.data[hex], out.voidData[hex]).
    // Re-check against tools/golden-dumper/README.md once the suite exists. Unknown shapes are skipped, not failed.
    for (const auto& c : suite.cases) {
        if (!c.in.contains("source") || !c.out.contains("data") || !c.out.contains("voidData")) {
            MESSAGE("f2r case '" << c.name << "' has an unexpected shape; skipped");
            continue;
        }
        const auto& s = c.in["source"];
        if (!s.contains("pos") || !s.contains("mapping") || !s.contains("columns")) continue;

        MapBlockRegistry reg(true);
        TestClientLevel  level;
        TransformOptions opt;
        FullDataSource   src = FullDataSource::createEmpty(test::hexU64(s["pos"]));
        src.isEmpty          = false;
        for (const auto& m : s["mapping"]) {
            src.mapping.addIfAbsent(
                reg.resolveBiomeOrDefault(m.value("biome", std::string())),
                reg.resolveBlock(m.value("block", std::string()))
            );
        }
        for (const auto& col : s["columns"]) {
            src.column(col.value("x", 0), col.value("z", 0)) = test::hexU64Array(col["data"]);
        }
        const auto& o             = c.in.contains("options") ? c.in["options"] : nlohmann::json::object();
        opt.verticalQuality       = static_cast<VerticalQuality>(o.value("verticalQuality", 2));
        opt.tintWithAvoidedBlocks = o.value("tintWithAvoidedBlocks", true);
        opt.enableCaveCulling     = o.value("enableCaveCulling", true);
        opt.caveCullingHeight     = o.value("caveCullingHeight", 60);
        opt.blocksToAvoid     = o.value("blocksToIgnore", 1) == 1 ? BlocksToAvoid::NonColliding : BlocksToAvoid::None;
        level.minY            = o.value("minHeight", -64);
        const BlockSet ign    = resolveCsv(o.value("ignoredRenderBlockCsv", std::string()), reg);
        const BlockSet cave   = resolveCsv(o.value("ignoredRenderCaveBlockCsv", std::string()), reg);
        const BlockSet sub    = resolveCsv(o.value("waterSubSurfaceBlockReplacementCsv", std::string()), reg);
        const BlockSet surf   = resolveCsv(o.value("waterSurfaceBlockReplacementCsv", std::string()), reg);
        opt.ignoredBlocks     = &ign;
        opt.ignoredCaveBlocks = &cave;
        opt.waterSubSurfaceReplace = &sub;
        opt.waterSurfaceReplace    = &surf;

        auto r = transformFullDataToRenderSource(src, level, reg, opt);
        REQUIRE(r.has_value());
        const auto expected = test::hexU64Array(c.out["data"]);
        CHECK_MESSAGE(r->data == expected, c.name);
        CHECK_MESSAGE(r->voidData == test::hexU64Array(c.out["voidData"]), c.name);
    }
}
