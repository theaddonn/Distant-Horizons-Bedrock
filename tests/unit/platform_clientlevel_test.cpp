#include <doctest.h>

#include "dh/platform/TableClientLevel.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace dh;

namespace {

std::string slurp(const char* path) {
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

BlockTable& table() {
    static BlockTable t = [] {
        auto r = BlockTable::load(slurp("../data/block_table.json"), slurp("../data/biome_table.json"));
        REQUIRE_MESSAGE(r.has_value(), "failed to load data/*.json");
        return std::move(r.value());
    }();
    return t;
}

TableClientLevel makeLevel() {
    LevelInfo li;
    li.minHeight     = -64;
    li.maxHeight     = 320;
    li.dimensionName = "minecraft:overworld";
    return TableClientLevel(table(), li);
}

} // namespace

TEST_CASE("TableClientLevel: level info passthrough") {
    LevelInfo li;
    li.minHeight     = 0;
    li.maxHeight     = 128;
    li.hasCeiling    = true;
    li.hasSkyLight   = false;
    li.dimensionName = "minecraft:the_nether";
    TableClientLevel lvl(table(), li);
    CHECK(lvl.minHeight() == 0);
    CHECK(lvl.maxHeight() == 128);
    CHECK(lvl.height() == 128);
    CHECK(lvl.hasCeiling());
    CHECK_FALSE(lvl.hasSkyLight());
    CHECK_FALSE(lvl.isEnd());
    CHECK(lvl.dimensionName() == "minecraft:the_nether");
    CHECK(lvl.dhIdentifier() == "minecraft:the_nether");
}

TEST_CASE("TableClientLevel: untinted, tinted and translucent colours") {
    TableClientLevel lvl    = makeLevel();
    BlockTable&      t      = table();
    const BiomeInfo* plains = t.resolveBiome("minecraft:plains");
    REQUIRE(plains);

    // untinted: table colour regardless of biome
    const BlockInfo* stone = t.resolveBlock("minecraft:stone");
    CHECK(lvl.blockColor(0, 0, 0, plains, stone) == t.colorInfo(stone)->argb);

    // grass_block 0xFF959595 * plains grass 0x92BC58 (per-channel x*y/255, truncating)
    const BlockInfo* grass = t.resolveBlock("minecraft:grass_block");
    CHECK(lvl.blockColor(0, 64, 0, plains, grass) == 0xFF556D33u);

    // foliage in plains
    const BlockInfo* leaves = t.resolveBlock("minecraft:oak_leaves");
    CHECK(lvl.blockColor(0, 64, 0, plains, leaves) == 0xFF45631Bu); // 0x949594 * 0x77AB2F

    // water keeps its alpha (0xF0), tinted by the biome water colour
    const BlockInfo* water = t.water();
    std::uint32_t    wc    = lvl.blockColor(0, 62, 0, plains, water);
    CHECK((wc >> 24) == 0xF0u);
    CHECK(((wc >> 16) & 0xFF) == (0xA5u * 0x44u / 255u));
    CHECK(((wc >> 8) & 0xFF) == (0xA5u * 0xAFu / 255u));
    CHECK((wc & 0xFF) == (0xA5u * 0xF5u / 255u));

    // null biome -> default biome tints (plains)
    CHECK(lvl.blockColor(0, 64, 0, nullptr, grass) == 0xFF556D33u);

    // air is invisible, a foreign block pointer yields 0
    CHECK((lvl.blockColor(0, 0, 0, plains, t.air()) >> 24) == 0u);
    BlockInfo foreign;
    CHECK(lvl.blockColor(0, 0, 0, plains, &foreign) == 0u);
}

TEST_CASE("TableClientLevel: dirt colour and shade constants") {
    TableClientLevel lvl = makeLevel();
    CHECK(lvl.dirtBlockColor() == 0xFF8A6346u);

    CHECK(lvl.shade(Direction::Down) == 0.5f);
    CHECK(lvl.shade(Direction::Up) == 1.0f);
    CHECK(lvl.shade(Direction::North) == 0.8f);
    CHECK(lvl.shade(Direction::South) == 0.8f);
    CHECK(lvl.shade(Direction::West) == 0.6f);
    CHECK(lvl.shade(Direction::East) == 0.6f);
}
