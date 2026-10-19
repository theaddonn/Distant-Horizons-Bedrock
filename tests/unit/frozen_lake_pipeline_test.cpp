// End-to-end check of a frozen lake column (ice over water over sand) through the real path:
// chunk -> full data -> sky bake -> render data -> mesh -> Stage-A vertices, using the real block table.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/mesh/StageAVertex.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/FrozenSurface.h"
#include "dh/platform/TableClientLevel.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/FullToRender.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Color.h"

using namespace dh;

namespace {

std::string slurpFile(const char* path) {
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

BlockTable& lakeTable() {
    static BlockTable t = [] {
        auto r = BlockTable::load(slurpFile("../data/block_table.json"), slurpFile("../data/biome_table.json"));
        REQUIRE_MESSAGE(r.has_value(), "failed to load data/*.json");
        return std::move(r.value());
    }();
    return t;
}

struct LakeResult {
    std::vector<StageAVertex>  verts;     // all vertices of the lake column (x=y=0 .. section), both lists
    std::vector<std::uint64_t> renderCol; // render datapoints of column (5,5)
    std::vector<std::uint64_t> fullCol;
};

/// stone -64..59, sand y=60, water 61..62, ice y=63, optional `top` block at y=64.
LakeResult buildLake(const char* top, bool glueFix = true) {
    BlockTable& t     = lakeTable();
    auto*       stone = t.resolveBlock("minecraft:stone");
    auto*       sand  = t.resolveBlock("minecraft:sand");
    auto*       water = t.resolveBlock("minecraft:water");
    auto*       ice   = t.resolveBlock("minecraft:ice");
    REQUIRE((stone && sand && water && ice));

    TestChunk c(0, 0, -64, 320, t.air(), t.defaultBiome());
    c.setDhBlockLightingCorrect(true);
    c.fillLayers(-64, 60, stone);
    c.fillLayer(60, sand);
    c.fillLayers(61, 63, water);
    c.fillLayer(63, ice);
    if (top != nullptr) {
        auto* b = t.resolveBlock(top);
        REQUIRE(b);
        c.fillLayer(64, b);
    }

    if (glueFix) {
        // what the Bedrock chunk adapter does before full-data conversion
        freezeWaterUnderSnowCoveredIce(
            -64,
            320,
            [&](int x, int y, int z) { return c.blockAt(x, y, z); },
            [&](int x, int y, int z, const BlockInfo* b) { c.setBlock(x, y, z, b); }
        );
    }
    auto fd = createFromChunk(c, BuildOptions{});
    REQUIRE(fd.has_value());
    bakeDataSourceSkyLight(*fd, 15);

    LakeResult r;
    r.fullCol = fd->column(5, 5);

    TableClientLevel level(t, LevelInfo{});
    TransformOptions to;
    auto             rs = transformFullDataToRenderSource(*fd, level, t, to);
    REQUIRE(rs.has_value());
    for (int i = 0; i < rs->maxVerticalSlices; i++) r.renderCol.push_back(rs->dataPoint(5, 5, i));

    MeshOptions mo;
    mo.quad.transparency = Transparency::Complete;
    LodQuadBuilder builder(mo.quad);
    REQUIRE(makeLodRenderData(builder, *rs, level, AdjacentSources{}, mo));
    MeshBuffers mb = builder.build();
    for (auto* list : {&mb.opaque, &mb.transparent})
        for (auto& buf : *list) convertDhBuffer(buf, 6, -64.0f, r.verts, Primitive::QuadList);
    return r;
}

} // namespace

namespace {

// Water/ice/snow region of the fixture: world y > 61. Nothing there may be near-black.
void checkNoBlackVertices(const LakeResult& r) {
    int checked = 0;
    for (const StageAVertex& v : r.verts) {
        if (v.y <= 61.0f) continue; // y=61 is the sand lake floor: sky 0 under ice, same as Java
        const int maxChannel = std::max(
            {static_cast<int>(v.rgba & 0xFF),
             static_cast<int>((v.rgba >> 8) & 0xFF),
             static_cast<int>((v.rgba >> 16) & 0xFF)}
        );
        // darkest legal result: a mid-grey surface (0x80) at the vanilla light floor of sky 0 / block 0
        CHECK_MESSAGE(maxChannel >= 40, "near-black vertex at y=" << v.y << " rgba=" << std::hex << v.rgba);
        checked++;
    }
    CHECK(checked > 0);
}

int waterDatapoints(const LakeResult& r) {
    int n = 0;
    for (auto d : r.renderCol)
        if (d != rdp::EMPTY_DATA && rdp::material(d) == BlockMaterial::Water) n++;
    return n;
}

} // namespace

TEST_CASE("frozen lake: ice over water over sand is ice coloured and lit") {
    LakeResult r = buildLake(nullptr);
    // water under ice is replaced by ice and merged into the ice datapoint (Java :443 + :691)
    REQUIRE(r.renderCol[0] != rdp::EMPTY_DATA);
    CHECK(rdp::yMax(r.renderCol[0]) == 128);
    CHECK(rdp::yMin(r.renderCol[0]) == 125);
    CHECK(rdp::skyLight(r.renderCol[0]) == 15);
    CHECK(waterDatapoints(r) == 0);
    checkNoBlackVertices(r);
}

TEST_CASE("frozen lake: snow layer on ice does not leave a dark water column") {
    LakeResult r = buildLake("minecraft:snow_layer[height=0]");
    CHECK(waterDatapoints(r) == 0);
    // the frozen water takes the ice, the snow is carried down onto it: one lit datapoint over the whole ice column
    CHECK(rdp::skyLight(r.renderCol[0]) == 15);
    CHECK(rdp::yMin(r.renderCol[0]) == 125);
    checkNoBlackVertices(r);
}

TEST_CASE("frozen lake: snow-covered ice renders as opaque white snow, not translucent ice (Run 13)") {
    BlockTable&         t        = lakeTable();
    TableClientLevel    level(t, LevelInfo{});
    const std::uint32_t snowArgb = level.blockColor(0, 0, 0, nullptr, t.resolveBlock("minecraft:snow_layer"));
    const std::uint32_t iceArgb  = level.blockColor(0, 0, 0, nullptr, t.resolveBlock("minecraft:ice"));
    REQUIRE(color::getAlpha(iceArgb) < 255); // ice is translucent: bare ice over a dark floor reads lavender

    LakeResult snowy = buildLake("minecraft:snow_layer[height=0]");
    REQUIRE(snowy.renderCol[0] != rdp::EMPTY_DATA);
    CHECK(rdp::argb(snowy.renderCol[0]) == color::setAlpha(snowArgb, 255));
    CHECK(rdp::material(snowy.renderCol[0]) == BlockMaterial::Snow);
    CHECK(rdp::yMax(snowy.renderCol[0]) == 128);

    // bare ice (no snow) is unchanged: Java's own rule, translucent ice colour
    LakeResult bare = buildLake(nullptr);
    CHECK((rdp::argb(bare.renderCol[0]) & 0xFFFFFFu) == (iceArgb & 0xFFFFFFu));
    CHECK(color::getAlpha(rdp::argb(bare.renderCol[0])) < 255); // render data quantises alpha
}

TEST_CASE("freezeWaterUnderSnowCoveredIce: only water directly under snow-covered ice") {
    BlockTable& t       = lakeTable();
    auto*       stone   = t.resolveBlock("minecraft:stone");
    auto*       water   = t.resolveBlock("minecraft:water");
    auto*       ice     = t.resolveBlock("minecraft:ice");
    auto*       packed  = t.resolveBlock("minecraft:packed_ice");
    auto*       snow    = t.resolveBlock("minecraft:snow_layer[height=1]");
    auto*       grass   = t.resolveBlock("minecraft:grass_block");
    auto*       seagras = t.resolveBlock("minecraft:seagrass");
    REQUIRE((stone && water && ice && packed && snow && grass && seagras));

    TestChunk c(0, 0, -64, 320, t.air(), t.defaultBiome());
    c.fillLayers(-64, 50, stone);
    c.fillLayers(50, 63, water);
    c.fillLayer(63, ice);
    c.fillLayer(64, snow);
    // (1,1): packed ice; a seagrass at y=58 stops the run
    c.setBlock(1, 63, 1, packed);
    c.setBlock(1, 58, 1, seagras);
    // (2,2): bare ice (no snow) -> untouched (Java's rule handles it)
    c.setBlock(2, 64, 2, t.air());
    // (3,3): snow on ice on land -> untouched
    for (int y = 50; y < 63; ++y) c.setBlock(3, y, 3, stone);
    // (4,4): snow on grass over water -> untouched
    c.setBlock(4, 63, 4, grass);

    const int n = freezeWaterUnderSnowCoveredIce(
        -64,
        320,
        [&](int x, int y, int z) { return c.blockAt(x, y, z); },
        [&](int x, int y, int z, const BlockInfo* b) { c.setBlock(x, y, z, b); }
    );
    // 252 plain columns with 13 water blocks each; (1,1): 4 water blocks (59..62) above the seagrass
    CHECK(n == (256 - 4) * 13 + 4);
    CHECK(c.blockAt(0, 64, 0) == snow); // the snow is kept
    CHECK(c.blockAt(0, 63, 0) == ice);
    CHECK(c.blockAt(0, 50, 0) == ice);
    CHECK(c.blockAt(0, 49, 0) == stone);
    CHECK(c.blockAt(1, 62, 1) == packed); // each column takes its own ice block
    CHECK(c.blockAt(1, 59, 1) == packed);
    CHECK(c.blockAt(1, 58, 1) == seagras);
    CHECK(c.blockAt(1, 57, 1) == water);
    CHECK(c.blockAt(2, 62, 2) == water);
    CHECK(c.blockAt(3, 62, 3) == stone);
    CHECK(c.blockAt(4, 62, 4) == water);
}

TEST_CASE("frozen lake: without the glue fix Java's rule misses (documents the root cause)") {
    LakeResult r = buildLake("minecraft:snow_layer[height=0]", false);
    // snow carry-over swaps the ice block, so the water below is NOT replaced and sits at sky light 0
    CHECK(waterDatapoints(r) == 1);
}

