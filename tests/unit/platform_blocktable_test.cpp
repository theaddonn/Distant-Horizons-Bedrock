#include <doctest.h>

#include "dh/platform/BlockTable.h"
#include "dh/platform/ChunkHeightUtil.h"

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

// Tests run with cwd = tests/ (set_rundir), data lives one level up.
BlockTable& table() {
    static BlockTable t = [] {
        std::string b = slurp("../data/block_table.json");
        std::string m = slurp("../data/biome_table.json");
        auto        r = BlockTable::load(b, m);
        REQUIRE_MESSAGE(r.has_value(), "failed to load data/*.json");
        return std::move(r.value());
    }();
    return t;
}

} // namespace

TEST_CASE("BlockTable: counts") {
    BlockTable& t = table();
    CHECK(t.blockCount() == 1523 + 8); // 1523 ids + 8 snow_layer height variants
    CHECK(t.biomeCount() == 89);
}

TEST_CASE("BlockTable: well-known entries") {
    BlockTable& t = table();

    const BlockInfo* stone = t.resolveBlock("minecraft:stone");
    REQUIRE(stone);
    CHECK(stone->isSolid);
    CHECK(stone->opacity == 16);
    CHECK_FALSE(stone->isAir);
    CHECK(stone->material == BlockMaterial::Stone);
    const BlockColorInfo* sc = t.colorInfo(stone);
    REQUIRE(sc);
    CHECK(sc->tint == TintKind::None);

    CHECK(t.air() == t.resolveBlock("minecraft:air"));
    CHECK(t.air()->isAir);
    CHECK(t.air()->opacity == 0);
    CHECK(t.water() == t.resolveBlock("minecraft:water"));
    CHECK(t.water()->isLiquid);
    CHECK(t.water()->opacity == 1);
    CHECK(t.colorInfo(t.water())->tint == TintKind::Water);

    const BlockInfo* grass = t.resolveBlock("minecraft:grass_block");
    REQUIRE(grass);
    const BlockColorInfo* gc = t.colorInfo(grass);
    REQUIRE(gc);
    CHECK(gc->argb == 0xFF959595u);
    CHECK(gc->down == 0xFF8A6346u);
    CHECK(gc->tint == TintKind::Grass);
    CHECK(gc->tintFaces == kTintFaceUp);

    const BlockInfo* leaves = t.resolveBlock("minecraft:oak_leaves");
    REQUIRE(leaves);
    CHECK(t.colorInfo(leaves)->tint == TintKind::Foliage);
    CHECK(t.colorInfo(leaves)->tintFaces == (kTintFaceUp | kTintFaceSide | kTintFaceDown));
    CHECK(leaves->material == BlockMaterial::Leaves);

    CHECK(t.colorInfo(t.resolveBlock("minecraft:birch_leaves"))->tint == TintKind::Birch);
    CHECK(t.colorInfo(t.resolveBlock("minecraft:spruce_leaves"))->tint == TintKind::Evergreen);
}

TEST_CASE("BlockTable: snow_layer state variants") {
    BlockTable&      t    = table();
    const BlockInfo* base = t.resolveBlock("minecraft:snow_layer");
    REQUIRE(base);
    const BlockInfo* h3 = t.resolveBlock("minecraft:snow_layer[height=3]");
    REQUIRE(h3);
    CHECK(h3 != base);
    CHECK(h3->serial == "minecraft:snow_layer[height=3]");
    CHECK(h3->material == BlockMaterial::Snow);
    CHECK(h3->opacity == base->opacity);
    CHECK(h3->isSolid == base->isSolid);
    CHECK(t.colorInfo(h3)->argb == t.colorInfo(base)->argb);

    // every height distinct
    for (int h = 0; h < 8; ++h) {
        const BlockInfo* v = t.resolveBlock("minecraft:snow_layer[height=" + std::to_string(h) + "]");
        REQUIRE(v);
        CHECK(v->serial.find("height=" + std::to_string(h)) != std::string::npos);
    }

    // unlisted state value / unlisted state key on a known block: base entry
    CHECK(t.resolveBlock("minecraft:snow_layer[height=9]") == base);
    CHECK(t.resolveBlock("minecraft:stone[stone_type=granite]") == t.resolveBlock("minecraft:stone"));

    auto keys = BlockTable::stateKeys("minecraft:snow_layer");
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "height");
    CHECK(BlockTable::stateKeys("minecraft:stone").empty());
}

TEST_CASE("BlockTable: interning is stable and unknown serials are nullptr") {
    BlockTable& t = table();
    CHECK(t.resolveBlock("minecraft:stone") == t.resolveBlock(std::string("minecraft:stone")));
    CHECK(t.resolveBlock("minecraft:snow_layer[height=3]") == t.resolveBlock("minecraft:snow_layer[height=3]"));
    CHECK(t.resolveBiome("minecraft:plains") == t.resolveBiome("minecraft:plains"));
    CHECK(t.resolveBiome("minecraft:plains") == t.defaultBiome());

    CHECK(t.resolveBlock("minecraft:does_not_exist") == nullptr);
    CHECK(t.resolveBlock("") == nullptr);
    CHECK(t.resolveBlock("minecraft:does_not_exist[a=1]") == nullptr);
    CHECK(t.resolveBiome("minecraft:nope") == nullptr);
    CHECK(t.resolveBlockOrAir("minecraft:does_not_exist") == t.air());
    CHECK(t.resolveBiomeOrDefault("minecraft:nope") == t.defaultBiome());

    BlockInfo foreign;
    CHECK(t.colorInfo(&foreign) == nullptr);
}

TEST_CASE("BlockTable: biome tints") {
    BlockTable&      t      = table();
    const BiomeInfo* plains = t.resolveBiome("minecraft:plains");
    REQUIRE(plains);
    const BiomeTint* bt = t.biomeTint(plains);
    REQUIRE(bt);
    CHECK(bt->grass == 0xFF92BC58u);
    CHECK(bt->foliage == 0xFF77AB2Fu);
    CHECK(bt->water == 0xFF44AFF5u);
    CHECK(bt->birch == 0xFF80A755u);
    CHECK(bt->evergreen == 0xFF619961u);
    CHECK(bt->dryFoliage == 0xFFA37546u);
    CHECK(t.biomeTint(nullptr) == bt);
}

TEST_CASE("BlockTable: load rejects malformed input") {
    CHECK_FALSE(BlockTable::load("not json", "{}").has_value());
    CHECK_FALSE(BlockTable::load("{\"blocks\":{}}", "{\"biomes\":{}}").has_value()); // no air/water
    CHECK_FALSE(BlockTable::load("[]", "{}").has_value());
}

TEST_CASE("ChunkHeightUtil: all-empty chunk keeps the Java quirk (min + 16)") {
    auto b = chunkheight::computeNonEmptyBounds(-64, 320, [](int) { return true; });
    CHECK(b.minNonEmpty == -64);
    CHECK(b.maxNonEmpty == -48);
    auto b2 = chunkheight::computeNonEmptyBounds(-64, 320, [](int s) { return s != 5; });
    CHECK(b2.minNonEmpty == 16);
    CHECK(b2.maxNonEmpty == 32);
}

TEST_CASE("BlockTable: resolveAllStates and ice flags") {
    BlockTable& t   = table();
    auto        all = t.resolveAllStates("minecraft:snow_layer");
    CHECK(all.size() == 9);
    CHECK(all[0] == t.resolveBlock("minecraft:snow_layer"));
    CHECK(t.resolveAllStates("minecraft:stone").size() == 1);
    CHECK(t.resolveAllStates("minecraft:nope").empty());
    for (const char* n : {"minecraft:ice", "minecraft:packed_ice", "minecraft:blue_ice", "minecraft:frosted_ice"})
        CHECK(t.resolveBlock(n)->isIce);
}
