#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "dh/build/LightingEngine.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/testing/TestChunk.h"
#include "golden_loader.h"

using namespace dh;

// NOTE on expected values: Java lightChunk stores the seed values into every chunk of the 3x3
// (center included), then clears the CENTER chunk and propagates from the queued seeds. The center's
// seed cells therefore start at 0 and are re-lit from their neighbours: a torch (15) in the center ends
// up as 13 at its own position, and open-sky cells of the center end up as 14 (neighbour chunks keep 15).
// The tests below encode this faithful behaviour (see LightingEngine.h).

namespace {

struct Blocks {
    MapBlockRegistry reg;
    const BlockInfo* air;
    const BlockInfo* water;
    const BlockInfo* stone;
    const BlockInfo* glass;
    const BlockInfo* torch;
    const BlockInfo* leaves;
    const BiomeInfo* biome;

    Blocks()
    : air(reg.air()),
      water(reg.water()),
      stone(reg.addBlock({.serial = "stone", .opacity = 16})),
      glass(reg.addBlock({.serial = "glass", .opacity = 0, .isAir = false, .isSolid = true})),
      torch(reg.addBlock({.serial = "torch", .opacity = 0, .lightEmission = 15, .isSolid = false})),
      leaves(reg.addBlock({.serial = "leaves", .opacity = 1})),
      biome(reg.defaultBiome()) {}
};

constexpr int kMinY = 0;
constexpr int kMaxY = 64;

/// 3x3 neighbourhood around chunk (0,0); index (dx+1) + (dz+1)*3.
struct World {
    Blocks&                                   b;
    std::array<std::unique_ptr<TestChunk>, 9> chunks;

    explicit World(Blocks& blocks, int minY = kMinY, int maxY = kMaxY, bool floorAndTop = true) : b(blocks) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto c = std::make_unique<TestChunk>(dx, dz, minY, maxY, b.air, b.biome);
                if (floorAndTop) {
                    c->fillLayer(minY, b.stone);
                    // marks the top section non-empty so sky seeds start at the top of the build range
                    c->setBlock(15, maxY - 1, 15, b.stone);
                }
                chunks[static_cast<std::size_t>((dx + 1) + (dz + 1) * 3)] = std::move(c);
            }
        }
    }

    TestChunk& at(int dx, int dz) { return *chunks[static_cast<std::size_t>((dx + 1) + (dz + 1) * 3)]; }
    TestChunk& center() { return at(0, 0); }

    /// Set a world-coordinate block (the chunk is chosen automatically, must be inside the 3x3).
    void set(int wx, int y, int wz, const BlockInfo* block) {
        const int cx = wx >> 4, cz = wz >> 4;
        at(cx, cz).setBlock(wx & 15, y, wz & 15, block);
    }

    void light(const LightingOptions& opts = {}) {
        std::vector<IChunk*> ptrs;
        for (auto& c : chunks) ptrs.push_back(c.get());
        lightChunk(center(), ptrs, opts);
    }
};

LightingOptions blockOnly() { return {.maxSkyLight = 15, .generateBlockLight = true, .generateSkyLight = false}; }
LightingOptions skyOnly() { return {.maxSkyLight = 15, .generateBlockLight = false, .generateSkyLight = true}; }

} // namespace

// ---------------------------------------------------------------------------
// block light
// ---------------------------------------------------------------------------

TEST_CASE("lighting: single torch in open air gives a Manhattan falloff sphere") {
    Blocks b;
    World  w(b);
    w.set(8, 40, 8, b.torch);
    w.light(blockOnly());

    TestChunk& c = w.center();
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 1; y < kMaxY; ++y) { // y == 0 is the stone floor
                const int d   = std::abs(x - 8) + std::abs(y - 40) + std::abs(z - 8);
                const int exp = d == 0 ? 13 : std::max(0, 15 - d); // 13: see NOTE at the top
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                REQUIRE(c.dhBlockLight(x, y, z) == exp);
            }
        }
    }
    CHECK(c.dhBlockLight(8, 0, 8) == 0); // inside the opaque floor
    CHECK(c.isDhBlockLightingCorrect());
    CHECK_FALSE(c.isDhSkyLightingCorrect()); // sky not requested
}

TEST_CASE("lighting: torch next to an opaque wall") {
    Blocks b;
    World  w(b);
    // full wall x = 10 through the three chunks of column x = 0
    for (int z = -16; z < 32; ++z)
        for (int y = 1; y < kMaxY; ++y) w.set(10, y, z, b.stone);
    w.set(8, 40, 8, b.torch);
    w.light(blockOnly());

    TestChunk& c = w.center();
    for (int z = 0; z < 16; ++z)
        for (int y = 1; y < kMaxY; ++y) {
            CHECK(c.dhBlockLight(10, y, z) == 0);
            for (int x = 11; x < 16; ++x) CHECK(c.dhBlockLight(x, y, z) == 0);
            // west of the wall nothing changes
            for (int x = 0; x < 10; ++x) {
                const int d   = std::abs(x - 8) + std::abs(y - 40) + std::abs(z - 8);
                const int exp = d == 0 ? 13 : std::max(0, 15 - d);
                REQUIRE(c.dhBlockLight(x, y, z) == exp);
            }
        }
}

TEST_CASE("lighting: torch in a neighbour chunk lights the center chunk") {
    Blocks b;
    World  w(b);
    w.at(-1, 0).setBlock(15, 40, 8, b.torch); // world x = -1
    w.light(blockOnly());

    TestChunk& c = w.center();
    CHECK(c.dhBlockLight(0, 40, 8) == 14);
    CHECK(c.dhBlockLight(3, 40, 8) == 11);
    CHECK(c.dhBlockLight(14, 40, 8) == 0); // distance 15 -> 0
    CHECK(c.dhBlockLight(0, 41, 9) == 12);
    // the neighbour keeps the torch value (it is only cleared for the center)
    CHECK(w.at(-1, 0).dhBlockLight(15, 40, 8) == 15);
    CHECK(c.isDhBlockLightingCorrect());
    CHECK_FALSE(w.at(-1, 0).isDhBlockLightingCorrect());
}

TEST_CASE("lighting: missing neighbours are tolerated, chunks outside the 3x3 are ignored") {
    Blocks    b;
    TestChunk center(0, 0, kMinY, kMaxY, b.air, b.biome);
    TestChunk far(5, 5, kMinY, kMaxY, b.air, b.biome);
    TestChunk east(1, 0, kMinY, kMaxY, b.air, b.biome);
    center.setBlock(15, 20, 4, b.torch);
    far.setBlock(0, 20, 0, b.torch);
    std::array<IChunk*, 4> nearby = {nullptr, &far, &east, &center};
    lightChunk(center, nearby, blockOnly());
    CHECK(center.dhBlockLight(14, 20, 4) == 14);
    CHECK(east.dhBlockLight(0, 20, 4) == 14); // propagated into the east chunk too
    CHECK(far.dhBlockLight(0, 20, 0) == 0);   // never touched
    CHECK(center.dhBlockLight(0, 20, 4) == 0);
}

TEST_CASE("lighting: block light is cleared on the center before propagation") {
    Blocks b;
    World  w(b);
    w.center().setDhBlockLight(3, 30, 3, 9); // stale value
    w.set(8, 40, 8, b.torch);
    w.light(blockOnly());
    CHECK(w.center().dhBlockLight(3, 30, 3) == std::max(0, 15 - (5 + 10 + 5)));
}

// ---------------------------------------------------------------------------
// sky light
// ---------------------------------------------------------------------------

TEST_CASE("lighting: cave under a sealed roof is dark, outside is bright") {
    Blocks b;
    World  w(b);
    for (int x = -16; x < 32; ++x)
        for (int z = -16; z < 32; ++z) w.set(x, 40, z, b.stone);
    w.light(skyOnly());

    TestChunk& c = w.center();
    for (int y = 1; y < 40; ++y) CHECK(c.dhSkyLight(8, y, 8) == 0);
    CHECK(w.at(1, 0).dhSkyLight(8, 20, 8) == 0);
    CHECK(w.at(1, 0).dhSkyLight(8, 41, 8) == 15);
    CHECK(c.dhSkyLight(8, 41, 8) == 14); // NOTE at the top
    CHECK(c.isDhSkyLightingCorrect());
    CHECK_FALSE(c.isDhBlockLightingCorrect());
}

TEST_CASE("lighting: glass (opacity 0, not air) passes sky light") {
    Blocks b;
    World  w(b);
    for (int x = -16; x < 32; ++x)
        for (int z = -16; z < 32; ++z) w.set(x, 40, z, b.glass);
    w.light(skyOnly());

    for (int y = 1; y < 63; ++y) {
        CHECK(w.at(1, 0).dhSkyLight(8, y, 8) == 15);
        CHECK(w.center().dhSkyLight(8, y, 8) == 14);
    }
}

TEST_CASE("lighting: water (opacity 1) attenuates sky light by one per block") {
    Blocks b;
    World  w(b);
    for (int x = -16; x < 32; ++x)
        for (int z = -16; z < 32; ++z) w.set(x, 40, z, b.water);
    w.light(skyOnly());

    // neighbour chunk: seeds stop at the water, then 15 - 1 inside, one less per block below
    TestChunk& n = w.at(1, 0);
    CHECK(n.dhSkyLight(8, 41, 8) == 15);
    CHECK(n.dhSkyLight(8, 40, 8) == 14);
    CHECK(n.dhSkyLight(8, 39, 8) == 13);
    CHECK(n.dhSkyLight(8, 38, 8) == 12);
    CHECK(n.dhSkyLight(8, 30, 8) == 4);
    // center: starts one lower (NOTE at the top)
    TestChunk& c = w.center();
    CHECK(c.dhSkyLight(8, 41, 8) == 14);
    // the queued center seed (15) lights the water below it to 14 although the cell above holds 14
    CHECK(c.dhSkyLight(8, 40, 8) == 14);
    CHECK(c.dhSkyLight(8, 39, 8) == 13);
    CHECK(c.dhSkyLight(8, 38, 8) == 12);
}

TEST_CASE("lighting: overhang is lit from the side") {
    Blocks b;
    World  w(b);
    // slab over all of chunk column x = -1 and the x < 8 half of chunk column x = 0
    for (int x = -16; x < 8; ++x)
        for (int z = -16; z < 32; ++z) w.set(x, 40, z, b.stone);
    w.light(skyOnly());

    TestChunk& c = w.center();
    // open center columns (x >= 8): 14 all the way down
    CHECK(c.dhSkyLight(8, 20, 8) == 14);
    CHECK(c.dhSkyLight(12, 5, 8) == 14);
    // under the slab: one less per block away from the open edge
    for (int x = 0; x < 8; ++x) {
        CAPTURE(x);
        CHECK(c.dhSkyLight(x, 20, 8) == 7 + x);
    }
    // directly above the slab everything is open
    CHECK(c.dhSkyLight(2, 41, 8) == 14);
}

TEST_CASE("lighting: maxSkyLight = 0 produces no sky seeds but still sets the flag") {
    Blocks b;
    World  w(b);
    w.light({.maxSkyLight = 0, .generateBlockLight = false, .generateSkyLight = true});
    CHECK(w.center().dhSkyLight(8, 20, 8) == 0);
    CHECK(w.center().isDhSkyLightingCorrect());
}

TEST_CASE("lighting: performance smoke, 16x384x16 chunk with 50 torches") {
    Blocks                                  b;
    std::vector<std::unique_ptr<TestChunk>> chunks;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            auto c = std::make_unique<TestChunk>(dx, dz, -64, 320, b.air, b.biome);
            c->fillLayers(-64, 64, b.stone);
            chunks.push_back(std::move(c));
        }
    TestChunk&   center = *chunks[4];
    std::mt19937 rng(1234);
    for (int i = 0; i < 50; ++i) {
        center.setBlock(
            static_cast<int>(rng() % 16),
            64 + static_cast<int>(rng() % 100),
            static_cast<int>(rng() % 16),
            b.torch
        );
    }
    std::vector<IChunk*> ptrs;
    for (auto& c : chunks) ptrs.push_back(c.get());

    const auto t0 = std::chrono::steady_clock::now();
    lightChunk(center, ptrs, {});
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("lightChunk 16x384x16, 50 torches, block+sky: " << ms << " ms");
    CHECK(center.isDhBlockLightingCorrect());
    CHECK(center.isDhSkyLightingCorrect());
}

// ---------------------------------------------------------------------------
// data source sky bake
// ---------------------------------------------------------------------------

namespace {

struct SourceFixture {
    Blocks         b;
    FullDataSource src = FullDataSource::createEmpty(0);
    int            airId, glassId, waterId, stoneId;

    SourceFixture() {
        airId   = src.mapping.addIfAbsent(b.biome, b.air);
        glassId = src.mapping.addIfAbsent(b.biome, b.glass);
        waterId = src.mapping.addIfAbsent(b.biome, b.water);
        stoneId = src.mapping.addIfAbsent(b.biome, b.stone);
    }

    static std::uint64_t        dp(int id, int height, int minY) { return fdp::encode(id, height, minY, 0, 0); }
    std::vector<std::uint64_t>& col(int x, int z) { return src.column(x, z); }
    int                         sky(int x, int z, std::size_t i) { return fdp::skyLight(src.column(x, z)[i]); }
};

} // namespace

TEST_CASE("skybake: single opaque column gets 15 at the top") {
    SourceFixture f;
    f.col(5, 5) = {SourceFixture::dp(f.stoneId, 10, 0)};
    bakeDataSourceSkyLight(f.src, 15);
    CHECK(f.sky(5, 5, 0) == 15);
    CHECK(f.col(6, 5).empty());
}

TEST_CASE("skybake: stale light is cleared first") {
    SourceFixture f;
    f.col(5, 5) = {fdp::encode(f.airId, 10, 10, 7, 9), fdp::encode(f.stoneId, 10, 0, 7, 9)};
    bakeDataSourceSkyLight(f.src, 15);
    CHECK(f.sky(5, 5, 0) == 15);
    CHECK(f.sky(5, 5, 1) == 15);                 // copied from the transparent one above
    CHECK(fdp::blockLight(f.col(5, 5)[0]) == 7); // block light untouched
}

TEST_CASE("skybake: transparent stack shares the top light, glass counts as transparent") {
    SourceFixture f;
    f.col(0, 0) = {
        SourceFixture::dp(f.airId, 10, 30),
        SourceFixture::dp(f.glassId, 10, 20),
        SourceFixture::dp(f.airId, 20, 0),
    };
    bakeDataSourceSkyLight(f.src, 12);
    CHECK(f.sky(0, 0, 0) == 12);
    CHECK(f.sky(0, 0, 1) == 12);
    CHECK(f.sky(0, 0, 2) == 12);
}

TEST_CASE("skybake: opaque under transparent, absorption is opacity * height") {
    SourceFixture f;
    // air / water (3 high, opacity 1) / stone
    f.col(0, 0) = {
        SourceFixture::dp(f.airId, 4, 6),
        SourceFixture::dp(f.waterId, 3, 3),
        SourceFixture::dp(f.stoneId, 3, 0),
    };
    // water 20 high absorbs everything
    f.col(1, 0) = {
        SourceFixture::dp(f.airId, 4, 26),
        SourceFixture::dp(f.waterId, 20, 6),
        SourceFixture::dp(f.stoneId, 6, 0),
    };
    // water exactly as thick as the light (15): absorption < light fails -> stays 0
    f.col(2, 0) = {
        SourceFixture::dp(f.airId, 4, 21),
        SourceFixture::dp(f.waterId, 15, 6),
        SourceFixture::dp(f.stoneId, 6, 0),
    };
    bakeDataSourceSkyLight(f.src, 15);
    CHECK(f.sky(0, 0, 0) == 15);
    CHECK(f.sky(0, 0, 1) == 15); // water below air: copy (water is NOT in airIDs, lit from above)
    CHECK(f.sky(0, 0, 2) == 12); // 15 - 1 * 3
    CHECK(f.sky(1, 0, 2) == 0);
    CHECK(f.sky(2, 0, 2) == 0);
}

TEST_CASE("skybake: horizontal spread only into overlapping transparent datapoints") {
    SourceFixture f;
    // A: open column
    f.col(0, 0) = {SourceFixture::dp(f.airId, 20, 0)};
    // B, C: stone roof over an air pocket, in a row away from A
    f.col(1, 0) = {SourceFixture::dp(f.stoneId, 10, 10), SourceFixture::dp(f.airId, 10, 0)};
    f.col(2, 0) = {SourceFixture::dp(f.stoneId, 10, 10), SourceFixture::dp(f.airId, 10, 0)};
    // D: pocket does not overlap A in Y (A spans [0,20), D's pocket [20,30))
    f.col(
        0,
        1
    ) = {SourceFixture::dp(f.airId, 5, 40), SourceFixture::dp(f.stoneId, 10, 30), SourceFixture::dp(f.airId, 10, 20)};
    // stone column E next to A is never lit by spreading
    f.col(0, 2) = {SourceFixture::dp(f.stoneId, 10, 30), SourceFixture::dp(f.stoneId, 30, 0)};
    bakeDataSourceSkyLight(f.src, 15);

    CHECK(f.sky(0, 0, 0) == 15);
    CHECK(f.sky(1, 0, 1) == 14);
    CHECK(f.sky(2, 0, 1) == 13);
    CHECK(f.sky(1, 0, 0) == 15); // top datapoint is always max
    CHECK(f.sky(0, 1, 0) == 15);
    CHECK(f.sky(0, 1, 1) == 15); // opaque directly below transparent: copy
    CHECK(f.sky(0, 1, 2) == 0);  // pocket [20,30) vs A [0,20): no Y overlap, opaque above it
    CHECK(f.sky(0, 2, 1) == 0);  // opaque below opaque: absorption 16*10 >= 15 -> 0
}

TEST_CASE("skybake: fuzz invariants") {
    std::mt19937 rng(20240601);
    for (int iter = 0; iter < 60; ++iter) {
        SourceFixture f;
        const int     ids[5] = {f.airId, f.glassId, f.waterId, f.stoneId, f.airId};
        const int     maxSky = (iter % 5 == 0) ? 7 : 15;
        constexpr int N      = 6;
        for (int x = 0; x < N; ++x) {
            for (int z = 0; z < N; ++z) {
                const int                  count = static_cast<int>(rng() % 6); // 0..5, empty columns allowed
                std::vector<std::uint64_t> bottomUp;
                int                        y = 0;
                for (int i = 0; i < count; ++i) {
                    const int h = 1 + static_cast<int>(rng() % 8);
                    bottomUp.push_back(SourceFixture::dp(ids[rng() % 5], h, y));
                    y += h;
                }
                std::reverse(bottomUp.begin(), bottomUp.end());
                f.col(x, z) = bottomUp;
            }
        }
        bakeDataSourceSkyLight(f.src, maxSky);

        auto isAir = [&](std::uint64_t d) { return fdp::id(d) == f.airId || fdp::id(d) == f.glassId; };
        for (int x = 0; x < N; ++x) {
            for (int z = 0; z < N; ++z) {
                const auto& col = f.col(x, z);
                for (std::size_t i = 0; i < col.size(); ++i) {
                    const int s = fdp::skyLight(col[i]);
                    REQUIRE(s >= 0);
                    REQUIRE(s <= 15);
                    REQUIRE(s <= maxSky);
                    if (i == 0) REQUIRE(s == maxSky);
                    // opaque datapoints never get brighter than the one above
                    if (i > 0 && !isAir(col[i])) REQUIRE(s <= fdp::skyLight(col[i - 1]));
                    // NOTE: the horizontal fixed-point property (neighbour >= light - 1) does NOT hold in Java:
                    // a later column iteration re-reads a transparent datapoint and overwrites its light with
                    // a copy of the (possibly darker) datapoint above, dropping spread light. Kept faithfully.
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// golden
// ---------------------------------------------------------------------------

// TODO: the light_chunk / light_ds suites are generated in parallel by tools/golden-dumper.
// Once they exist, re-check the case key names against tools/golden-dumper/README.md and
// replace the placeholders below with real comparisons.
TEST_CASE("golden: light_chunk") {
    DH_GOLDEN_SUITE_OR_SKIP(suite, "light_chunk");
    CHECK_FALSE(suite.cases.empty());
}

TEST_CASE("golden: light_ds") {
    DH_GOLDEN_SUITE_OR_SKIP(suite, "light_ds");
    CHECK_FALSE(suite.cases.empty());
}
