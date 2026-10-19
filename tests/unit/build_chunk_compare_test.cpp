// dh::compareChunks: the generator compare-mode metric (Run 13).
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "dh/build/ChunkCompare.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/testing/TestChunk.h"

using namespace dh;

namespace {

BlockTable& table() {
    static BlockTable t = [] {
        auto read = [](const char* p) {
            std::ifstream     in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        auto r = BlockTable::load(read("../data/block_table.json"), read("../data/biome_table.json"));
        REQUIRE_MESSAGE(r.has_value(), "failed to load data/*.json");
        return std::move(r.value());
    }();
    return t;
}

TestChunk ground() {
    BlockTable& t = table();
    TestChunk   c(3, -2, -64, 320, t.air(), t.defaultBiome());
    c.fillLayers(-64, 62, t.resolveBlock("minecraft:stone"));
    c.fillLayer(62, t.resolveBlock("minecraft:grass_block"));
    return c;
}

} // namespace

TEST_CASE("compareChunks: identical chunks") {
    TestChunk a = ground(), b = ground();
    ChunkDiff d = compareChunks(a, b);
    CHECK(d.columns == 256);
    CHECK(d.sameTopY == 256);
    CHECK(d.sameTopBlock == 256);
    CHECK(d.diffBlocks == 0);
    CHECK(d.treeIoU() == doctest::Approx(1.0));
}

TEST_CASE("compareChunks: trees, snow and ice") {
    BlockTable& t      = table();
    auto*       log    = t.resolveBlock("minecraft:spruce_log");
    auto*       leaves = t.resolveBlock("minecraft:spruce_leaves");
    auto*       snow   = t.resolveBlock("minecraft:snow_layer");
    auto*       ice    = t.resolveBlock("minecraft:ice");
    REQUIRE((log && leaves && snow && ice));
    REQUIRE(log->material == BlockMaterial::Wood);
    REQUIRE(leaves->material == BlockMaterial::Leaves);

    TestChunk real = ground(), gen = ground();
    // real: trees at columns (0,0) and (1,0); gen: trees at (1,0) and (2,0)
    real.setBlock(0, 63, 0, log);
    real.setBlock(1, 63, 0, leaves);
    gen.setBlock(1, 63, 0, leaves);
    gen.setBlock(2, 63, 0, log);
    // real has snow on (5,5), gen has nothing there; both have ice on (6,6)
    real.setBlock(5, 63, 5, snow);
    real.setBlock(6, 62, 6, ice);
    gen.setBlock(6, 62, 6, ice);

    ChunkDiff d = compareChunks(real, gen);
    CHECK(d.treeReal == 2);
    CHECK(d.treeGen == 2);
    CHECK(d.treeBoth == 1);
    CHECK(d.treeIoU() == doctest::Approx(1.0 / 3.0));
    CHECK(d.snowReal == 1);
    CHECK(d.snowGen == 0);
    CHECK(d.iceReal == 1);
    CHECK(d.iceGen == 1);
    CHECK(d.diffBlocks == 3); // (0,63,0) (2,63,0) (5,63,5)
    CHECK(d.sameTopY == 256 - 3);
    CHECK(d.sameTopBlock == 256 - 3);

    ChunkDiff sum;
    sum.add(d);
    sum.add(d);
    CHECK(sum.columns == 512);
    CHECK(sum.treeBoth == 2);
    CHECK(sum.summary().find("IoU 0.33") != std::string::npos);
}
