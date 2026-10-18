#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/fulldata/Merge.h"
#include "dh/fulldata/OcclusionCuller.h"
#include "dh/pos/SectionPos.h"

using namespace dh;

namespace {

struct World {
    BiomeInfo biome{"plains"};
    BlockInfo air, stone, dirt, glass;
    World() {
        air.serial    = "air";
        air.isAir     = true;
        air.opacity   = 0;
        stone.serial  = "stone";
        dirt.serial   = "dirt";
        glass.serial  = "glass";
        glass.opacity = 3;
    }
    const BlockInfo* blk(int i) const {
        const BlockInfo* all[] = {&air, &stone, &dirt, &glass};
        return all[i];
    }
};

using Src = std::unique_ptr<FullDataSource>;

Src mk(std::int8_t detail, int x, int z) {
    return std::make_unique<FullDataSource>(FullDataSource::createEmpty(sec::encode(detail, x, z)));
}

/// Adds the 4 blocks of `w` in order air, stone, dirt, glass -> ids 0..3.
void addBlocks(FullDataSource& s, const World& w) {
    for (int i = 0; i < 4; i++) s.mapping.addIfAbsent(&w.biome, w.blk(i));
}

std::uint64_t dp(int id, int minY, int height, int bl = 0, int sl = 15) {
    return fdp::encode(id, height, minY, static_cast<std::uint8_t>(bl), static_cast<std::uint8_t>(sl));
}

void put(FullDataSource& s, int x, int z, std::vector<std::uint64_t> col, int gen = 5, int comp = 0) {
    s.setSingleColumn(std::move(col), x, z, static_cast<std::int8_t>(gen), static_cast<std::int8_t>(comp));
    s.isEmpty = false;
}

const BlockInfo* blockOf(const FullDataSource& s, std::uint64_t d) { return s.mapping.block(fdp::id(d)); }

} // namespace

TEST_CASE("merge: empty input mapping returns false and changes nothing") {
    World w;
    Src   self = mk(6, 0, 0), in = mk(6, 0, 0);
    addBlocks(*self, w);
    put(*self, 0, 0, {dp(1, 0, 10)});
    put(*in, 0, 0, {dp(0, 0, 5)}); // mapping empty
    auto r = updateFromDataSource(*self, *in, {});
    REQUIRE(r.has_value());
    CHECK_FALSE(*r);
    CHECK(self->mapping.size() == 4); // not even renumbered
    CHECK(self->column(0, 0) == std::vector<std::uint64_t>{dp(1, 0, 10)});
}

TEST_CASE("merge: only same / one below / one above detail accepted") {
    World w;
    Src   in = mk(6, 0, 0);
    addBlocks(*in, w);
    put(*in, 0, 0, {dp(1, 0, 10)});
    for (int d : {6, 7, 5}) {
        Src self = mk(static_cast<std::int8_t>(d), 0, 0);
        CHECK(updateFromDataSource(*self, *in, {}).has_value());
    }
    for (int d : {4, 8, 9}) {
        Src self = mk(static_cast<std::int8_t>(d), 0, 0);
        CHECK_FALSE(updateFromDataSource(*self, *in, {}).has_value());
    }
}

TEST_CASE("merge: same level generation step gate") {
    World w;
    struct C {
        int  thisGen, inputGen;
        bool replaced;
    };
    const C cases[] = {
        {0,  -1, true },
        {-1, -1, true },
        {3,  -1, false},
        {5,  -1, false}, // input DOWN_SAMPLED
        {0,  0,  false},
        {-1, 0,  false}, // input EMPTY never replaces
        {3,  3,  true },
        {2,  3,  true },
        {4,  3,  false},
        {-1, 3,  true },
        {0,  3,  true },
    };
    for (const C& c : cases) {
        Src self = mk(6, 0, 0), in = mk(6, 0, 0);
        addBlocks(*self, w);
        addBlocks(*in, w);
        put(*self, 5, 5, {dp(1, 0, 10)}, c.thisGen);
        put(*in, 5, 5, {dp(2, 0, 10)}, c.inputGen);
        auto r = updateFromDataSource(*self, *in, {});
        REQUIRE(r.has_value());
        const auto& col = self->column(5, 5);
        REQUIRE(col.size() == 1);
        CAPTURE(c.thisGen);
        CAPTURE(c.inputGen);
        CHECK((blockOf(*self, col[0]) == &w.dirt) == c.replaced);
        CHECK(*r == c.replaced);
        CHECK(self->genStepAt(5, 5) == (c.replaced ? c.inputGen : c.thisGen));
    }
}

TEST_CASE("merge: same level dataChanged and compression overwrite") {
    World w;
    Src   self = mk(6, 0, 0), in = mk(6, 0, 0);
    addBlocks(*self, w);
    addBlocks(*in, w);
    put(*self, 1, 1, {dp(1, 0, 10)}, 3, 0);
    put(*in, 1, 1, {dp(1, 0, 10)}, 3, 1);
    auto r = updateFromDataSource(*self, *in, {});
    CHECK_FALSE(*r);                    // identical data
    CHECK(self->compModeAt(1, 1) == 1); // but compression mode is always overwritten
    in->column(1, 1)[0] = dp(1, 0, 11);
    r                   = updateFromDataSource(*self, *in, {});
    CHECK(*r);
    CHECK(self->column(1, 1)[0] == dp(0, 0, 11)); // id renumbered by removeUnusedIdsAndRemap (stone -> 0)
}

TEST_CASE("merge: dataChanged compares against un-remapped input ids (Java quirk)") {
    World w;
    Src   self = mk(6, 0, 0), in = mk(6, 0, 0);
    self->mapping.addIfAbsent(&w.biome, &w.dirt);
    self->mapping.addIfAbsent(&w.biome, &w.stone); // stone = 1
    addBlocks(*in, w);                             // stone = 1 as well
    put(*self, 0, 0, {dp(1, 0, 5)}, 3);
    put(*in, 0, 0, {dp(1, 0, 5)}, 3);
    CHECK_FALSE(*updateFromDataSource(*self, *in, {})); // ids coincide
    Src self2 = mk(6, 0, 0);
    self2->mapping.addIfAbsent(&w.biome, &w.stone); // stone = 0 here
    put(*self2, 0, 0, {dp(0, 0, 5)}, 3);
    CHECK(*updateFromDataSource(*self2, *in, {})); // same block, but 0 != 1 -> reported as changed
    CHECK(self2->column(0, 0) == std::vector<std::uint64_t>{dp(0, 0, 5)});
}

TEST_CASE("merge: flags") {
    World w;
    auto  run = [&](int detail, std::optional<bool> a, std::optional<bool> b, auto field) {
        Src self = mk(static_cast<std::int8_t>(detail), 0, 0), in = mk(static_cast<std::int8_t>(detail), 0, 0);
        addBlocks(*in, w);
        put(*in, 0, 0, {dp(1, 0, 1)});
        field(*self) = a;
        field(*in)   = b;
        updateFromDataSource(*self, *in, {});
        return field(*self);
    };
    auto parent = [](FullDataSource& s) -> std::optional<bool>& { return s.applyToParent; };
    auto child  = [](FullDataSource& s) -> std::optional<bool>& { return s.applyToChildren; };
    auto regen  = [](FullDataSource& s) -> std::optional<bool>& { return s.regenerateLeaf; };

    CHECK(run(8, std::nullopt, std::nullopt, parent) == std::nullopt);
    CHECK(run(8, std::nullopt, true, parent) == std::optional<bool>(true));
    CHECK(run(8, false, std::nullopt, parent) == std::optional<bool>(false));
    CHECK(run(14, true, true, parent) == std::optional<bool>(false)); // not past the root
    CHECK(run(13, std::nullopt, true, parent) == std::optional<bool>(true));

    CHECK(run(6, true, true, child) == std::optional<bool>(false)); // not below the leaf
    CHECK(run(7, std::nullopt, true, child) == std::optional<bool>(true));
    CHECK(run(7, std::nullopt, std::nullopt, child) == std::nullopt);

    CHECK(run(6, std::nullopt, true, regen) == std::optional<bool>(true));
    CHECK(run(14, true, false, regen) == std::optional<bool>(true)); // no level clamp
    CHECK(run(9, std::nullopt, std::nullopt, regen) == std::nullopt);

    // parent / child level updates leave the flags alone
    Src self = mk(7, 0, 0), in = mk(6, 0, 0);
    addBlocks(*in, w);
    put(*in, 0, 0, {dp(1, 0, 1)});
    in->applyToParent = true;
    updateFromDataSource(*self, *in, {});
    CHECK_FALSE(self->applyToParent.has_value());
}

TEST_CASE("merge: parent from four children (offsets, gen step min, compression max)") {
    World w;
    Src   parent = mk(7, 0, 0);
    // children (x,z): offsets (0,0) (32,0) (0,32) (32,32)
    const int cx[] = {0, 1, 0, 1}, cz[] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
        Src ch = mk(6, cx[i], cz[i]);
        addBlocks(*ch, w);
        for (int x = 0; x < 64; x++)
            for (int z = 0; z < 64; z++) put(*ch, x, z, {dp(1 + i % 2, 0, 8)}, 4, -1);
        // the 2x2 block at (2,4) carries distinct gen steps and compression modes
        put(*ch, 2, 4, {dp(1, 0, 8)}, 7, 0);
        put(*ch, 3, 4, {dp(1, 0, 8)}, -1, 1);
        put(*ch, 2, 5, {dp(1, 0, 8)}, 5, 0);
        put(*ch, 3, 5, {dp(1, 0, 8)}, 9, 0);
        auto r = updateFromDataSource(*parent, *ch, {});
        REQUIRE(r.has_value());
        CHECK(*r);
    }
    CHECK(parent->genStepAt(1, 2) == -1); // min over the 2x2 block
    CHECK(parent->compModeAt(1, 2) == 1); // max over the 2x2 block
    CHECK(parent->genStepAt(0, 0) == 4);
    CHECK(parent->compModeAt(0, 0) == -1); // INT8_MIN start, max of four -1
    // child 1 = (1,0) lands at x offset 32; child 2 at z offset 32
    CHECK(blockOf(*parent, parent->column(0, 0)[0]) == &w.stone);
    CHECK(blockOf(*parent, parent->column(32, 0)[0]) == &w.dirt);
    CHECK(blockOf(*parent, parent->column(0, 32)[0]) == &w.stone);
    CHECK(blockOf(*parent, parent->column(32, 32)[0]) == &w.dirt);
    CHECK_FALSE(parent->isEmpty);
    CHECK(parent->mapping.size() == 2); // only stone and dirt remain
}

namespace {
/// Builds a child with four sub-columns at (0,0),(0,1),(1,0),(1,1) and merges into a fresh parent.
std::vector<std::uint64_t> mergeFour(
    const World&                      w,
    const std::vector<std::uint64_t>& c0,
    const std::vector<std::uint64_t>& c1,
    const std::vector<std::uint64_t>& c2,
    const std::vector<std::uint64_t>& c3,
    Src*                              outParent = nullptr
) {
    Src ch     = mk(6, 0, 0);
    Src parent = mk(7, 0, 0);
    addBlocks(*ch, w);
    put(*ch, 0, 0, c0);
    put(*ch, 0, 1, c1);
    put(*ch, 1, 0, c2);
    put(*ch, 1, 1, c3);
    auto r = updateFromDataSource(*parent, *ch, {});
    REQUIRE(r.has_value());
    auto col = parent->column(0, 0);
    if (outParent) *outParent = std::move(parent);
    return col;
}
} // namespace

TEST_CASE("merge: slice vote - air never wins, ties go to the lowest index") {
    World w;
    Src   p;
    // three air + one stone -> stone
    auto col = mergeFour(w, {dp(0, 0, 10)}, {dp(0, 0, 10)}, {dp(1, 0, 10)}, {dp(0, 0, 10)}, &p);
    REQUIRE(col.size() == 1);
    CHECK(blockOf(*p, col[0]) == &w.stone);
    CHECK(fdp::height(col[0]) == 10);

    // all air -> value0 (air)
    col = mergeFour(w, {dp(0, 0, 10)}, {dp(0, 0, 10)}, {dp(0, 0, 10)}, {dp(0, 0, 10)}, &p);
    REQUIRE(col.size() == 1);
    CHECK(blockOf(*p, col[0]) == &w.air);

    // tie stone / dirt (+2 air): lowest index (stone at index 0)
    col = mergeFour(w, {dp(1, 0, 10)}, {dp(2, 0, 10)}, {dp(0, 0, 10)}, {dp(0, 0, 10)}, &p);
    CHECK(blockOf(*p, col[0]) == &w.stone);
    // same with the order swapped
    col = mergeFour(w, {dp(2, 0, 10)}, {dp(1, 0, 10)}, {dp(0, 0, 10)}, {dp(0, 0, 10)}, &p);
    CHECK(blockOf(*p, col[0]) == &w.dirt);

    // air at index 0 and a three-way split: value0 = air has count 0; counts 1/1/1 -> maxCount == count0? no (0) ->
    // value1
    col = mergeFour(w, {dp(0, 0, 10)}, {dp(1, 0, 10)}, {dp(2, 0, 10)}, {dp(3, 0, 10)}, &p);
    CHECK(blockOf(*p, col[0]) == &w.stone);

    // majority wins over index order
    col = mergeFour(w, {dp(1, 0, 10)}, {dp(2, 0, 10)}, {dp(2, 0, 10)}, {dp(0, 0, 10)}, &p);
    CHECK(blockOf(*p, col[0]) == &w.dirt);

    // literal count3 quirk: the 4th distinct value lands in count3 even if it equals value3 only
    // [stone, dirt, glass, glass]: c0=1 c1=1 c2=2 -> glass
    col = mergeFour(w, {dp(1, 0, 10)}, {dp(2, 0, 10)}, {dp(3, 0, 10)}, {dp(3, 0, 10)}, &p);
    CHECK(blockOf(*p, col[0]) == &w.glass);
}

TEST_CASE("merge: slice light averaging only over the winner, truncating") {
    World w;
    Src   p;
    // stone light (block 3, sky 4) and (block 4, sky 9), dirt (15, 15), air (0,0): winner stone
    auto col =
        mergeFour(w, {dp(1, 0, 10, 3, 4)}, {dp(1, 0, 10, 4, 9)}, {dp(2, 0, 10, 15, 15)}, {dp(0, 0, 10, 0, 0)}, &p);
    REQUIRE(col.size() == 1);
    CHECK(fdp::blockLight(col[0]) == 3); // (3+4)/2 truncated
    CHECK(fdp::skyLight(col[0]) == 6);   // (4+9)/2 truncated
}

TEST_CASE("merge: slices, runs and ordering") {
    World w;
    Src   p;
    // all four: stone [0,5), dirt [5,10) given top-down -> two runs, output top-down
    std::vector<std::uint64_t> c   = {dp(2, 5, 5), dp(1, 0, 5)};
    auto                       col = mergeFour(w, c, c, c, c, &p);
    REQUIRE(col.size() == 2);
    CHECK(fdp::minY(col[0]) == 5);
    CHECK(blockOf(*p, col[0]) == &w.dirt);
    CHECK(fdp::minY(col[1]) == 0);
    CHECK(blockOf(*p, col[1]) == &w.stone);
    CHECK_FALSE(FullDataSource::isColumnInWrongOrder(col));

    // same id but different light splits the run
    c   = {dp(1, 5, 5, 0, 7), dp(1, 0, 5, 0, 9)};
    col = mergeFour(w, c, c, c, c, &p);
    REQUIRE(col.size() == 2);
    CHECK(fdp::skyLight(col[0]) == 7);
    CHECK(fdp::skyLight(col[1]) == 9);

    // same id and light but split: merged into one run
    c   = {dp(1, 5, 5), dp(1, 0, 5)};
    col = mergeFour(w, c, c, c, c, &p);
    REQUIRE(col.size() == 1);
    CHECK(fdp::height(col[0]) == 10);

    // empty columns -> empty column
    col = mergeFour(w, {}, {}, {}, {}, &p);
    CHECK(col.empty());

    // uncovered slices read id 0 (air in this mapping): sub-columns staggered
    col = mergeFour(w, {dp(1, 0, 5)}, {}, {}, {dp(1, 5, 5)}, &p);
    // slice [0,5): ids [1,0,0,0]; air skipped -> only value0=stone counted -> stone
    // slice [5,10): ids [0,0,0,1] -> stone (the only non-air)
    REQUIRE(col.size() == 1);
    CHECK(blockOf(*p, col[0]) == &w.stone);
    CHECK(fdp::height(col[0]) == 10);

    // slice sampling truncates: transitions 0,3 -> sample at 1
    col = mergeFour(w, {dp(1, 0, 3)}, {dp(1, 0, 1)}, {}, {}, &p);
    // transitions {0,1,3}: slice [0,1) sample 0; slice [1,3) sample 2 -> only c0 covers
    REQUIRE(col.size() == 1);
    CHECK(fdp::height(col[0]) == 3);
}

TEST_CASE("merge: downsample from one above") {
    World w;
    Src   parent = mk(7, 0, 0);
    addBlocks(*parent, w);
    for (int x = 0; x < 64; x++)
        for (int z = 0; z < 64; z++) put(*parent, x, z, {dp(1, 0, 20)}, 5, 0);
    // distinguish compression at the recipient index from the one at the input index
    parent->columnWorldCompressionMode[static_cast<std::size_t>(FullDataSource::relativePosToIndex(0, 0))]   = 1;
    parent->columnWorldCompressionMode[static_cast<std::size_t>(FullDataSource::relativePosToIndex(40, 50))] = 1;
    parent->column(40, 50)[0] = dp(2, 0, 20);

    Src self = mk(6, 1, 1); // child (1,1): offsets 32,32
    addBlocks(*self, w);
    put(*self, 2, 2, {dp(3, 0, 4)}, 8, 0);           // existing real data must be kept
    put(*self, 4, 4, {fdp::EMPTY_DATA_POINT}, 0, 0); // all-empty column is replaced

    auto r = updateFromDataSource(*self, *parent, {});
    REQUIRE(r.has_value());
    CHECK(*r);
    CHECK_FALSE(self->isEmpty);
    // untouched real data
    REQUIRE(self->column(2, 2).size() == 1);
    CHECK(blockOf(*self, self->column(2, 2)[0]) == &w.glass);
    // filled from input (x/2+32, z/2+32) = (34,34): stone
    REQUIRE(self->column(4, 4).size() == 1);
    CHECK(blockOf(*self, self->column(4, 4)[0]) == &w.stone);
    CHECK(fdp::height(self->column(4, 4)[0]) == 20);
    // (16,36) -> input (40,50): dirt
    CHECK(blockOf(*self, self->column(16, 36)[0]) == &w.dirt);
    // gen step always DOWN_SAMPLED, even for columns that were kept
    CHECK(self->genStepAt(2, 2) == -1);
    CHECK(self->genStepAt(0, 0) == -1);
    // quirk: compression read at recipientIndex of the input
    CHECK(self->compModeAt(0, 0) == 1);
    CHECK(self->compModeAt(40, 50) == 1);
    CHECK(self->compModeAt(1, 1) == 0);

    // second run: everything now has data -> no change, but flags still set
    Src self2 = mk(6, 0, 0);
    addBlocks(*self2, w);
    for (int x = 0; x < 64; x++)
        for (int z = 0; z < 64; z++) put(*self2, x, z, {dp(3, 0, 4)}, 8, 0);
    r = updateFromDataSource(*self2, *parent, {});
    CHECK_FALSE(*r);
    CHECK(self2->genStepAt(10, 10) == -1);
    CHECK(blockOf(*self2, self2->column(10, 10)[0]) == &w.glass);
}

TEST_CASE("merge: removeUnusedIdsAndRemap renumbers in first-use order") {
    World w;
    Src   s = mk(6, 0, 0);
    addBlocks(*s, w); // air 0, stone 1, dirt 2, glass 3
    // scan order: x outer, z inner, top -> bottom
    put(*s, 0, 1, {dp(2, 10, 5), dp(3, 0, 10)});
    put(*s, 1, 0, {dp(1, 0, 5)});
    removeUnusedIdsAndRemap(*s);
    REQUIRE(s->mapping.size() == 3);
    CHECK(s->mapping.block(0) == &w.dirt); // (0,1) top
    CHECK(s->mapping.block(1) == &w.glass);
    CHECK(s->mapping.block(2) == &w.stone); // (1,0)
    CHECK(fdp::id(s->column(0, 1)[0]) == 0);
    CHECK(fdp::id(s->column(0, 1)[1]) == 1);
    CHECK(fdp::id(s->column(1, 0)[0]) == 2);
    // idempotent
    auto before = s->columns;
    removeUnusedIdsAndRemap(*s);
    CHECK(s->columns == before);
}

TEST_CASE("merge: renumbering runs even when nothing changed") {
    World w;
    Src   self = mk(6, 0, 0), in = mk(6, 0, 0);
    addBlocks(*self, w);
    addBlocks(*in, w);
    put(*self, 0, 0, {dp(3, 0, 5)}, 5);
    put(*in, 0, 0, {dp(3, 0, 5)}, 5);
    auto r = updateFromDataSource(*self, *in, {});
    CHECK_FALSE(*r);
    CHECK(self->mapping.size() == 1);
    CHECK(fdp::id(self->column(0, 0)[0]) == 0);
}

//========//
// culler //
//========//

namespace {
Src cullFixture(const World& w, std::vector<std::uint64_t> center, std::vector<std::uint64_t> neighbour) {
    Src s = mk(6, 0, 0);
    addBlocks(*s, w);
    put(*s, 1, 1, std::move(center));
    put(*s, 2, 1, neighbour);
    put(*s, 0, 1, neighbour);
    put(*s, 1, 2, neighbour);
    put(*s, 1, 0, neighbour);
    return s;
}
} // namespace

TEST_CASE("cull: fully covered middle datapoint is absorbed by the one above") {
    World w;
    Src   s = cullFixture(w, {dp(1, 20, 10), dp(2, 10, 10), dp(1, 0, 10)}, {dp(1, 0, 40)});
    cullHiddenDatapointsInColumn(*s, 1, 1);
    const auto& c = s->column(1, 1);
    REQUIRE(c.size() == 2);
    CHECK(fdp::minY(c[0]) == 10);
    CHECK(fdp::height(c[0]) == 20);
    CHECK(fdp::minY(c[1]) == 0);
}

TEST_CASE("cull: skip conditions") {
    World                            w;
    const std::vector<std::uint64_t> three = {dp(1, 20, 10), dp(2, 10, 10), dp(1, 0, 10)};
    {
        // an empty neighbour disables culling for the whole column
        Src s = cullFixture(w, three, {dp(1, 0, 40)});
        s->column(2, 1).clear();
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1) == three);
    }
    {
        // out of bounds neighbour (edge column)
        Src s = mk(6, 0, 0);
        addBlocks(*s, w);
        put(*s, 0, 0, three);
        cullHiddenDatapointsInColumn(*s, 0, 0);
        CHECK(s->column(0, 0) == three);
    }
    {
        // translucent neighbour datapoint -> visible
        Src s = cullFixture(w, three, {dp(3, 0, 40)});
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1) == three);
    }
    {
        // Java quirk: a neighbour that does NOT reach the top of the middle point makes the scan run off
        // the bottom; ~(-1) == 0 is non-negative so it reads as "covered" and the point IS culled.
        Src s = cullFixture(w, three, {dp(1, 0, 15)});
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1).size() == 2);
    }
    {
        // middle point adjacent to a translucent point of its own column
        std::vector<std::uint64_t> col = {dp(3, 20, 10), dp(2, 10, 10), dp(1, 0, 10)};
        Src                        s   = cullFixture(w, col, {dp(1, 0, 40)});
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1) == col);
        col = {dp(1, 20, 10), dp(2, 10, 10), dp(3, 0, 10)};
        s   = cullFixture(w, col, {dp(1, 0, 40)});
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1) == col);
    }
    {
        // two datapoints only: top and lowest are never culled
        std::vector<std::uint64_t> col = {dp(1, 10, 10), dp(2, 0, 10)};
        Src                        s   = cullFixture(w, col, {dp(1, 0, 40)});
        cullHiddenDatapointsInColumn(*s, 1, 1);
        CHECK(s->column(1, 1) == col);
    }
}

TEST_CASE("cull: checkOcclusion bitwise-not encoding and the fall-through quirk") {
    World w;
    Src   s = mk(6, 0, 0);
    addBlocks(*s, w);
    const std::uint64_t center = dp(1, 20, 10);
    CHECK(checkOcclusion(*s, center, {}, 3) == ~3);
    CHECK(checkOcclusion(*s, center, {fdp::EMPTY_DATA_POINT}, 0) == ~0);
    CHECK(checkOcclusion(*s, center, {dp(1, 0, 40)}, 0) == 0);                // covers
    CHECK(checkOcclusion(*s, center, {dp(3, 0, 40)}, 0) == ~0);               // translucent
    CHECK(checkOcclusion(*s, center, {dp(1, 25, 10), dp(1, 0, 25)}, 1) == 0); // idx1 top 25 < 30, idx0 top 35 covers
    CHECK(checkOcclusion(*s, center, {dp(1, 40, 10), dp(1, 0, 35)}, 1) == 1);
    // adjacent column that ends below the centre: loop runs off, returns ~(-1) == 0 (reads as "covered")
    CHECK(checkOcclusion(*s, center, {dp(1, 0, 10)}, 0) == 0);
    // partial overlap that ends within the centre: also runs off (index -1 -> 0)
    CHECK(checkOcclusion(*s, center, {dp(1, 0, 25)}, 0) == 0);
}

TEST_CASE("merge: culling only runs when compression != MERGE_SAME_BLOCKS and data changed") {
    World w;
    auto  build = [&](Src& self, Src& in) {
        self = mk(6, 0, 0);
        in   = mk(6, 0, 0);
        addBlocks(*in, w);
        for (int x = 0; x < 3; x++)
            for (int z = 0; z < 3; z++) put(*in, x, z, {dp(1, 0, 40)});
        put(*in, 1, 1, {dp(1, 20, 10), dp(2, 10, 10), dp(1, 0, 10)});
    };
    Src self, in;
    build(self, in);
    updateFromDataSource(*self, *in, {WorldCompressionMode::MergeSameBlocks});
    CHECK(self->column(1, 1).size() == 3);
    build(self, in);
    updateFromDataSource(*self, *in, {WorldCompressionMode::VisuallyEqual});
    CHECK(self->column(1, 1).size() == 2);
}

//======//
// fuzz //
//======//

TEST_CASE("merge fuzz: parent-from-children invariants") {
    World           w;
    std::mt19937_64 rng(0xB2B2);
    auto            rnd = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };

    for (int iter = 0; iter < 40; iter++) {
        Src              parent = mk(7, 0, 0);
        const int        cx[] = {0, 1, 0, 1}, cz[] = {0, 0, 1, 1};
        std::vector<Src> kids;
        for (int k = 0; k < 4; k++) {
            Src ch = mk(6, cx[k], cz[k]);
            addBlocks(*ch, w);
            for (int x = 0; x < 64; x++) {
                for (int z = 0; z < 64; z++) {
                    // random top-down column with random gaps, may be empty
                    std::vector<std::uint64_t> col;
                    int                        y = rnd(40);
                    const int                  n = rnd(5);
                    std::vector<std::uint64_t> up;
                    for (int i = 0; i < n; i++) {
                        const int h = 1 + rnd(12);
                        up.push_back(dp(rnd(4), y, h, rnd(16), rnd(16)));
                        y += h + (rnd(3) == 0 ? 1 + rnd(5) : 0);
                    }
                    col.assign(up.rbegin(), up.rend());
                    put(*ch, x, z, col, 1 + rnd(8), rnd(2));
                }
            }
            auto r = updateFromDataSource(*parent, *ch, {});
            REQUIRE(r.has_value());
            kids.push_back(std::move(ch));
        }

        // content idempotence: merging the last child again leaves every column unchanged.
        // (The returned dataChanged flag is NOT reliable here: Java compares old datapoints against the
        // not-yet-remapped input ids, so it reports a change whenever the id spaces differ.)
        {
            const auto before = parent->columns;
            auto       r      = updateFromDataSource(*parent, *kids[3], {});
            REQUIRE(r.has_value());
            CHECK(parent->columns == before);
        }

        // check the last child's quadrant (offset 32,32) against a brute-force oracle
        const FullDataSource& ch = *kids[3];
        for (int x = 0; x < 64; x += 2) {
            for (int z = 0; z < 64; z += 2) {
                const auto& out = parent->column(x / 2 + 32, z / 2 + 32);
                // order + no overlap
                for (std::size_t i = 0; i + 1 < out.size(); i++) {
                    REQUIRE(fdp::minY(out[i]) >= fdp::minY(out[i + 1]) + fdp::height(out[i + 1]));
                }
                for (std::uint64_t d : out) {
                    REQUIRE(fdp::id(d) < static_cast<int>(parent->mapping.size()));
                    REQUIRE(fdp::height(d) > 0);
                }
                const std::vector<std::uint64_t>* in4[4] =
                    {&ch.column(x, z), &ch.column(x, z + 1), &ch.column(x + 1, z), &ch.column(x + 1, z + 1)};
                std::vector<int> ys;
                for (auto* c : in4)
                    for (std::uint64_t d : *c) {
                        ys.push_back(fdp::minY(d));
                        ys.push_back(fdp::minY(d) + fdp::height(d));
                    }
                if (ys.empty()) {
                    CHECK(out.empty());
                    continue;
                }
                std::sort(ys.begin(), ys.end());
                ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
                // no gaps: output spans [ys.front, ys.back)
                REQUIRE_FALSE(out.empty());
                CHECK(fdp::minY(out.back()) == ys.front());
                CHECK(fdp::minY(out.front()) + fdp::height(out.front()) == ys.back());
                for (std::size_t i = 0; i + 1 < out.size(); i++)
                    CHECK(fdp::minY(out[i]) == fdp::minY(out[i + 1]) + fdp::height(out[i + 1]));
                // air never wins when a non-air candidate exists at the slice sample
                for (std::size_t i = 0; i + 1 < ys.size(); i++) {
                    const int sampleY = ys[i] + (ys[i + 1] - ys[i]) / 2;
                    bool      nonAir  = false;
                    for (auto* c : in4)
                        for (std::uint64_t d : *c)
                            if (fdp::minY(d) <= sampleY && sampleY < fdp::minY(d) + fdp::height(d)
                                && !ch.mapping.block(fdp::id(d))->isAir)
                                nonAir = true;
                    for (std::uint64_t d : out) {
                        if (fdp::minY(d) <= sampleY && sampleY < fdp::minY(d) + fdp::height(d)) {
                            if (nonAir) CHECK_FALSE(blockOf(*parent, d)->isAir);
                            CHECK(fdp::blockLight(d) <= 15);
                        }
                    }
                }
            }
        }

        // mapping holds exactly the used ids, in first-use order
        std::vector<int> firstUse;
        for (int x = 0; x < 64; x++)
            for (int z = 0; z < 64; z++)
                for (std::uint64_t d : parent->column(x, z))
                    if (std::find(firstUse.begin(), firstUse.end(), fdp::id(d)) == firstUse.end())
                        firstUse.push_back(fdp::id(d));
        for (std::size_t i = 0; i < firstUse.size(); i++) REQUIRE(firstUse[i] == static_cast<int>(i));
        CHECK(firstUse.size() == parent->mapping.size());
    }
}
