#include <doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dh/build/LodDataBuilder.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/pos/SectionPos.h"
#include "dh/util/Bits.h"
#include "golden_loader.h"

using namespace dh;

namespace {

using Col = std::vector<std::uint64_t>;

constexpr int MIN_Y = -64;
constexpr int MAX_Y = 64;

struct World {
    MapBlockRegistry reg;
    const BlockInfo* air;
    const BlockInfo* stone;
    const BlockInfo* grass;
    const BlockInfo* stairs;
    const BlockInfo* snow;
    const BlockInfo* flower;
    const BlockInfo* ice;
    const BlockInfo* water;
    const BiomeInfo* plains;
    const BiomeInfo* desert;

    World() {
        air     = reg.air();
        water   = reg.water();
        auto mk = [&](const char* s, std::uint8_t opacity, bool solid, bool ice = false) {
            BlockInfo b;
            b.serial  = s;
            b.opacity = opacity;
            b.isSolid = solid;
            b.isIce   = ice;
            return reg.addBlock(b);
        };
        stone  = mk("stone", 16, true);
        grass  = mk("grass_block", 16, true);
        stairs = mk("stairs", 16, false);
        snow   = mk("snow_layer", 1, false);
        flower = mk("poppy", 0, false);
        ice    = mk("ice", 3, true, true);
        plains = reg.defaultBiome();
        desert = reg.addBiome(BiomeInfo{"desert"});
    }

    TestChunk chunk(int cx, int cz, int minY = MIN_Y, int maxY = MAX_Y) {
        TestChunk c(cx, cz, minY, maxY, air, plains);
        c.setDhBlockLightingCorrect(true);
        return c;
    }
};

void skyAbove(TestChunk& c, int y0) {
    for (int y = y0; y < c.exclusiveMaxBuildHeight(); ++y)
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) c.setDhSkyLight(x, y, z, 15);
}

using fdp::encode;

/// Asserts every column of the chunk's 16x16 area for which `expected(x, z)` is given, and that
/// the rest of the section is untouched.
template <class F>
void checkSection(const FullDataSource& ds, int cx, int cz, F expected, int genStep = 9, int comp = 0) {
    const int offX = chunkOffsetInSection(cx);
    const int offZ = chunkOffsetInSection(cz);
    for (int x = 0; x < 64; ++x) {
        for (int z = 0; z < 64; ++z) {
            const bool inChunk = x >= offX && x < offX + 16 && z >= offZ && z < offZ + 16;
            if (inChunk) {
                const Col want = expected(x - offX, z - offZ);
                CHECK(ds.column(x, z) == want);
                CHECK(ds.genStepAt(x, z) == genStep);
                CHECK(ds.compModeAt(x, z) == comp);
            } else {
                CHECK(ds.column(x, z).empty());
                CHECK(ds.genStepAt(x, z) == 0);
            }
        }
    }
}

auto same(const Col& c) {
    return [c](int, int) { return c; };
}

} // namespace

TEST_CASE("build: flat chunk") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 9, w.stone);
    c.fillLayer(9, w.grass);
    skyAbove(c, 10);

    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    CHECK(ds->pos == sec::encode(6, 0, 0));
    CHECK(ds->isEmpty == false);
    CHECK(ds->applyToParent == std::optional<bool>(true));
    CHECK(ds->mapping.size() == 3);
    CHECK(ds->mapping.block(0) == w.air);
    CHECK(ds->mapping.block(1) == w.grass);
    CHECK(ds->mapping.block(2) == w.stone);
    CHECK(ds->mapping.biome(2) == w.plains);

    // air 64..9 (h 55, relMin of y=10 -> 74), grass y=9 (light of y=10), stone to the bottom
    checkSection(*ds, 0, 0, same({encode(0, 55, 74, 0, 15), encode(1, 1, 73, 0, 15), encode(2, 73, 0, 0, 0)}));
}

TEST_CASE("build: compression mode is stored per column") {
    World w;
    auto  c = w.chunk(1, 2);
    c.fillLayers(MIN_Y, 9, w.stone);
    auto ds = createFromChunk(c, BuildOptions{WorldCompressionMode::VisuallyEqual});
    REQUIRE(ds.has_value());
    CHECK(ds->compModeAt(16, 32) == 1);
    CHECK(ds->compModeAt(0, 0) == 0);
    CHECK(ds->genStepAt(16, 32) == static_cast<int>(GenerationStep::Light));
}

TEST_CASE("build: stairs (non-solid opaque) do not force a single block") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 9, w.stone);
    c.fillLayer(9, w.stairs);
    skyAbove(c, 10);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    // lightBlocking top (9) > solid top (8); opacity 16 -> forceSingleBlock stays false
    checkSection(*ds, 0, 0, same({encode(0, 55, 74, 0, 15), encode(1, 1, 73, 0, 15), encode(2, 73, 0, 0, 0)}));
}

TEST_CASE("build: snow layer on grass uses forceSingleBlock") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 8, w.stone);
    c.fillLayer(8, w.grass);
    c.fillLayer(9, w.grass);
    c.fillLayer(10, w.snow);
    skyAbove(c, 11);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    // grass at y=8 is a separate entry only because the snow above forces a split
    checkSection(
        *ds,
        0,
        0,
        same({
            encode(0, 54, 75, 0, 15), // air
            encode(1, 1, 74, 0, 15),  // snow y=10
            encode(2, 1, 73, 0, 0),   // grass y=9 (forced single)
            encode(2, 1, 72, 0, 0),   // grass y=8
            encode(3, 72, 0, 0, 0),   // stone
        })
    );
}

TEST_CASE("build: flower field (climb includes non-heightmap blocks)") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 9, w.stone);
    c.fillLayer(9, w.grass);
    skyAbove(c, 11);
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            if ((x + z) % 2 == 0) c.setBlock(x, 10, z, w.flower);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    const Col
        flowerCol{encode(0, 54, 75, 0, 15), encode(1, 1, 74, 0, 15), encode(2, 1, 73, 0, 0), encode(3, 73, 0, 0, 0)};
    const Col grassCol{encode(0, 55, 74, 0, 15), encode(2, 1, 73, 0, 0), encode(3, 73, 0, 0, 0)};
    checkSection(*ds, 0, 0, [&](int x, int z) { return (x + z) % 2 == 0 ? flowerCol : grassCol; });
}

TEST_CASE("build: water over ice") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 6, w.stone);
    c.fillLayer(6, w.ice);
    c.fillLayer(7, w.water);
    c.fillLayer(8, w.water);
    skyAbove(c, 9);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    checkSection(
        *ds,
        0,
        0,
        same({
            encode(0, 56, 73, 0, 15), // air
            encode(1, 2, 71, 0, 15),  // water y=7..8
            encode(2, 1, 70, 0, 0),   // ice y=6
            encode(3, 70, 0, 0, 0),   // stone
        })
    );
}

TEST_CASE("build: water column") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, -10, w.stone);
    c.fillLayers(-10, 0, w.water);
    skyAbove(c, 0);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    checkSection(*ds, 0, 0, same({encode(0, 65, 64, 0, 15), encode(1, 10, 54, 0, 15), encode(2, 54, 0, 0, 0)}));
}

TEST_CASE("build: cave with air pockets and block light") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 20, w.stone);
    c.fillLayers(5, 9, w.air);
    skyAbove(c, 20);
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z) c.setDhBlockLight(x, 9, z, 7);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    checkSection(
        *ds,
        0,
        0,
        same({
            encode(0, 45, 84, 0, 15), // air above
            encode(1, 11, 73, 0, 15), // stone y=9..19
            encode(0, 4, 69, 7, 0),   // pocket y=5..8 (light read at y=9)
            encode(1, 69, 0, 0, 0),   // stone below
        })
    );
}

TEST_CASE("build: floating block is cut at minNonEmptyHeight") {
    World w;
    auto  c = w.chunk(0, 0);
    c.setBlock(3, 30, 4, w.stone);
    skyAbove(c, 31);
    CHECK(c.minNonEmptyHeight() == 16);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    // only column (3,4) has a block; the rest are all air from the top down to minNonEmpty - 1
    const Col lone{encode(0, 34, 95, 0, 15), encode(1, 1, 94, 0, 15), encode(0, 14, 80, 0, 0)};
    checkSection(*ds, 0, 0, [&](int x, int z) { return (x == 3 && z == 4) ? lone : Col{encode(0, 49, 80, 0, 15)}; });
}

TEST_CASE("build: all-air chunk keeps isEmpty=false and one air point per column") {
    World w;
    auto  c = w.chunk(0, 0);
    CHECK(c.minNonEmptyHeight() == MIN_Y);
    CHECK(c.maxNonEmptyHeight() == MIN_Y + 16); // Java quirk documented in IChunk.h
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    CHECK(ds->isEmpty == false); // set up front (:74); the later non-empty check is dead code
    // loop runs y = -64 only, ends at y=-65: height 64-(-65), relMinY 0
    checkSection(*ds, 0, 0, same({encode(0, 129, 0, 0, 15)}));
}

TEST_CASE("build: biome changes split columns") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, 10, w.stone);
    for (int y = 5; y < 10; ++y)
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z) c.setBiome(x, y, z, w.desert);
    skyAbove(c, 10);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    CHECK(ds->mapping.biome(1) == w.desert);
    CHECK(ds->mapping.biome(2) == w.plains);
    checkSection(*ds, 0, 0, same({encode(0, 55, 74, 0, 15), encode(1, 5, 69, 0, 15), encode(2, 69, 0, 0, 0)}));
}

TEST_CASE("build: chunk offsets and section positions with negative coords") {
    CHECK(chunkOffsetInSection(5) == 16);
    CHECK(chunkOffsetInSection(4) == 0);
    CHECK(chunkOffsetInSection(3) == 48);
    CHECK(chunkOffsetInSection(0) == 0);
    CHECK(chunkOffsetInSection(-1) == 48);
    CHECK(chunkOffsetInSection(-2) == 32);
    CHECK(chunkOffsetInSection(-3) == 16);
    CHECK(chunkOffsetInSection(-4) == 0);
    CHECK(chunkOffsetInSection(-5) == 48);

    CHECK(sectionPosForChunk(-5, -1) == sec::encode(6, -2, -1));
    CHECK(sectionPosForChunk(-4, 3) == sec::encode(6, -1, 0));
    CHECK(sectionPosForChunk(-1, 4) == sec::encode(6, -1, 1));
    // agrees with floor division everywhere in a window (the Java trunc-then-minus-1 formula)
    for (int c = -40; c <= 40; ++c) {
        CHECK(sectionPosForChunk(c, 0) == sec::encode(6, jfloordiv(c, 4), 0));
        CHECK(chunkOffsetInSection(c) == jfloormod(c, 4) * 16);
    }
}

TEST_CASE("build: negative chunk coordinates land in the right columns") {
    World w;
    for (auto [cx, cz, sx, sz] : std::vector<std::array<int, 4>>{
             {-5, -1, -2, -1},
             {-1, -4, -1, -1},
             {-4, -5, -1, -2}
    }) {
        auto c = w.chunk(cx, cz);
        c.fillLayers(MIN_Y, 9, w.stone);
        skyAbove(c, 9);
        auto ds = createFromChunk(c, BuildOptions{});
        REQUIRE(ds.has_value());
        CHECK(ds->pos == sec::encode(6, sx, sz));
        checkSection(*ds, cx, cz, same({encode(0, 56, 73, 0, 15), encode(1, 73, 0, 0, 15)}));
    }
}

TEST_CASE("build: minY 0 and minY -64") {
    World w;
    {
        auto c = w.chunk(0, 0, 0, 64);
        c.fillLayers(0, 10, w.stone);
        skyAbove(c, 10);
        auto ds = createFromChunk(c, BuildOptions{});
        REQUIRE(ds.has_value());
        // relMinY is relative to inclusiveMin = 0
        checkSection(*ds, 0, 0, same({encode(0, 55, 10, 0, 15), encode(1, 10, 0, 0, 15)}));
    }
    {
        auto c = w.chunk(0, 0, -64, 320);
        c.fillLayers(-64, 70, w.stone);
        skyAbove(c, 70);
        auto ds = createFromChunk(c, BuildOptions{});
        REQUIRE(ds.has_value());
        // top y = 69; air point h = 320 - 69, relMinY = 70 + 64
        checkSection(*ds, 0, 0, same({encode(0, 251, 134, 0, 15), encode(1, 134, 0, 0, 15)}));
    }
}

TEST_CASE("build: column solid up to the build limit") {
    World w;
    auto  c = w.chunk(0, 0);
    c.fillLayers(MIN_Y, MAX_Y, w.stone);
    auto ds = createFromChunk(c, BuildOptions{});
    REQUIRE(ds.has_value());
    // climb ends at y == exclusiveMax (out-of-range read is air); no air point is emitted,
    // stone starts at lastY=64: y=63 differs from the initial air -> air point h=1.
    checkSection(*ds, 0, 0, same({encode(0, 1, 128, 0, 15), encode(1, 128, 0, 0, 15)}));
}

TEST_CASE("build: golden chunk_build") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "chunk_build.pending");
    // TODO(re-check against tools/golden-dumper/README.md once chunk_build is dumped): the input layout
    // below is a best guess - in.chunk.blocks = {palette:[{serial,isAir,isSolid,isLiquid,opacity}|serial],
    // columns:[256 x [[paletteIdx,count],...]] bottom-up from minY, index x*16+z}. Cases that do not match
    // are skipped rather than failed.
    for (const auto& gc : s.cases) {
        const auto& in = gc.in;
        if (!in.contains("chunk") || !in["chunk"].contains("blocks")) continue;
        const auto& jc = in["chunk"];
        const auto& jb = jc["blocks"];
        if (!jb.contains("palette") || !jb.contains("columns") || !jb["columns"].is_array()
            || jb["columns"].size() != 256)
            continue;

        MapBlockRegistry              reg;
        std::vector<const BlockInfo*> palette;
        for (const auto& e : jb["palette"]) {
            if (e.is_string()) {
                const std::string ser = e.get<std::string>();
                palette.push_back(
                    ser == "minecraft:air"     ? reg.air()
                    : ser == "minecraft:water" ? reg.water()
                                               : reg.addBlock(BlockInfo{ser})
                );
            } else {
                BlockInfo b;
                b.serial   = e.value("serial", std::string());
                b.isAir    = e.value("isAir", false);
                b.isSolid  = e.value("isSolid", true);
                b.isLiquid = e.value("isLiquid", false);
                b.opacity  = static_cast<std::uint8_t>(e.value("opacity", 16));
                palette.push_back(b.serial == "minecraft:air" ? reg.air() : reg.addBlock(b));
            }
        }
        const int cx = jc.value("chunkX", 0), cz = jc.value("chunkZ", 0);
        const int minY = jc.value("minY", MIN_Y), maxY = jc.value("maxY", MAX_Y);
        TestChunk chunk(cx, cz, minY, maxY, reg.air(), reg.defaultBiome());
        chunk.setDhBlockLightingCorrect(true);
        for (int i = 0; i < 256; ++i) {
            int y = minY;
            for (const auto& run : jb["columns"][static_cast<std::size_t>(i)]) {
                const int idx = run[0].get<int>(), n = run[1].get<int>();
                for (int k = 0; k < n && y < maxY; ++k, ++y)
                    chunk.setBlock(i / 16, y, i % 16, palette[static_cast<std::size_t>(idx)]);
            }
        }
        const auto comp = compressionModeFromValue(in.value("worldCompression", 0));
        if (!comp) continue;
        auto ds = createFromChunk(chunk, BuildOptions{*comp});
        if (!gc.out.contains("columns")) continue;
        REQUIRE_MESSAGE(ds.has_value(), gc.name);
        const auto& cols = gc.out["columns"];
        REQUIRE(cols.size() == 4096);
        for (int i = 0; i < 4096; ++i)
            CHECK_MESSAGE(
                ds->columns[static_cast<std::size_t>(i)] == dh::test::hexU64Array(cols[static_cast<std::size_t>(i)]),
                gc.name
            );
        if (gc.out.contains("isEmpty")) CHECK(ds->isEmpty == gc.out["isEmpty"].get<bool>());
    }
}
