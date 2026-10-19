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
        stripSnowOverFrozenWater(
            -64,
            320,
            t.air(),
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
    // snow is dropped, ice + frozen water merge into one lit datapoint
    CHECK(rdp::skyLight(r.renderCol[0]) == 15);
    CHECK(rdp::yMin(r.renderCol[0]) == 125);
    checkNoBlackVertices(r);
}

TEST_CASE("frozen lake: without the glue fix Java's rule misses (documents the root cause)") {
    LakeResult r = buildLake("minecraft:snow_layer[height=0]", false);
    // snow carry-over swaps the ice block, so the water below is NOT replaced and sits at sky light 0
    CHECK(waterDatapoints(r) == 1);
}
