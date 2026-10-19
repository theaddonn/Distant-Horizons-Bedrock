// End-to-end check of a frozen lake column (ice over water over sand) through the real path:
// chunk -> full data -> sky bake -> render data -> mesh -> Stage-A vertices, using the real block table.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dh/build/LodDataBuilder.h"
#include "dh/build/LightingEngine.h"
#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/mesh/StageAVertex.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/TableClientLevel.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/pos/SectionPos.h"
#include "dh/fulldata/FullDataPoint.h"
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
    std::vector<StageAVertex>  verts;      // all vertices of the lake column (x=y=0 .. section), both lists
    std::vector<std::uint64_t> renderCol;  // render datapoints of column (5,5)
    std::vector<std::uint64_t> fullCol;
};

/// stone -64..59, sand y=60, water 61..62, ice y=63, optional `top` block at y=64.
LakeResult buildLake(const char* top) {
    BlockTable& t = lakeTable();
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

TEST_CASE("frozen lake: diagnostics") {
    for (const char* top : {static_cast<const char*>(nullptr), "minecraft:snow_layer"}) {
        LakeResult r = buildLake(top);
        std::printf("== top=%s\n", top ? top : "(air)");
        for (auto d : r.fullCol)
            std::printf("  full id=%d y=%d h=%d sky=%d blk=%d\n", fdp::id(d), fdp::minY(d), fdp::height(d),
                        fdp::skyLight(d), fdp::blockLight(d));
        for (auto d : r.renderCol)
            if (d != rdp::EMPTY_DATA)
                std::printf("  rd top=%d bot=%d argb=%08x sky=%d blk=%d mat=%d\n", rdp::yMax(d), rdp::yMin(d),
                            rdp::argb(d), rdp::skyLight(d), rdp::blockLight(d), (int)rdp::material(d));
    }
}
