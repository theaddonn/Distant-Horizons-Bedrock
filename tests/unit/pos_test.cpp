#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "dh/pos/BlockPos.h"
#include "dh/pos/ChunkPos.h"
#include "dh/pos/Direction.h"
#include "dh/pos/MinMaxPos.h"
#include "dh/pos/SectionPos.h"

using namespace dh;
using sec::Pos;

namespace {

// DhSectionPosTest.assertSectionPosEqual
void assertSectionPosEqual(Pos expected, Pos actual) {
    INFO("expected: [" << sec::toString(expected) << "] actual: [" << sec::toString(actual) << "]");
    CHECK(sec::detail(expected) == sec::detail(actual));
    CHECK(sec::x(expected) == sec::x(actual));
    CHECK(sec::z(expected) == sec::z(actual));
}

constexpr std::int8_t BLOCK_LVL  = sec::SECTION_BLOCK_DETAIL_LEVEL;
constexpr std::int8_t CHUNK_LVL  = sec::SECTION_CHUNK_DETAIL_LEVEL;
constexpr std::int8_t REGION_LVL = sec::SECTION_REGION_DETAIL_LEVEL;

} // namespace

TEST_CASE("section pos constants match DhSectionPos.java / LodUtil.java") {
    CHECK(sec::SECTION_MINIMUM_DETAIL_LEVEL == 6);
    CHECK(sec::SECTION_BLOCK_DETAIL_LEVEL == 6);
    CHECK(sec::SECTION_CHUNK_DETAIL_LEVEL == 10);
    CHECK(sec::SECTION_REGION_DETAIL_LEVEL == 15);
    CHECK(sec::DETAIL_LEVEL_WIDTH == 8);
    CHECK(sec::X_POS_WIDTH == 28);
    CHECK(sec::Z_POS_WIDTH == 28);
    CHECK(sec::DETAIL_LEVEL_OFFSET == 0);
    CHECK(sec::POS_X_OFFSET == 8);
    CHECK(sec::POS_Z_OFFSET == 36);
    CHECK(sec::DETAIL_LEVEL_MASK == 127);
    CHECK(sec::POS_X_MASK == 0x0FFFFFFF);
    CHECK(sec::POS_Z_MASK == 0x0FFFFFFF);
}

//==================================//
// DhSectionPosTest.java (15 cases) //
//==================================//

TEST_CASE("DhSectionPosTest.basicEncodeDecodeTest") {
    Pos pos;

    // zero pos
    pos = sec::encode(0, 0, 0);
    CHECK(sec::detail(pos) == 0);
    CHECK(sec::x(pos) == 0);
    CHECK(sec::z(pos) == 0);

    // positive values
    pos = sec::encode(10, 4, 1);
    CHECK(sec::detail(pos) == 10);
    CHECK(sec::x(pos) == 4);
    CHECK(sec::z(pos) == 1);

    // negative position, positive detail level
    pos = sec::encode(2, -1, -4);
    CHECK(sec::detail(pos) == 2);
    CHECK(sec::x(pos) == -1);
    CHECK(sec::z(pos) == -4);
}

TEST_CASE("DhSectionPosTest.containsPosTest") {
    Pos root  = sec::encode(10, 0, 0);
    Pos child = sec::encode(9, 1, 1);

    CHECK(sec::contains(root, child));
    CHECK_FALSE(sec::contains(child, root));

    root = sec::encode(10, 1, 0);

    // out of bounds
    child = sec::encode(9, 0, 0);
    CHECK_FALSE(sec::contains(root, child));
    child = sec::encode(9, 1, 1);
    CHECK_FALSE(sec::contains(root, child));

    // in bounds
    child = sec::encode(9, 2, 0);
    CHECK(sec::contains(root, child));
    child = sec::encode(9, 3, 1);
    CHECK(sec::contains(root, child));

    // out of bounds
    child = sec::encode(9, 2, 2);
    CHECK_FALSE(sec::contains(root, child));
    child = sec::encode(9, 3, 3);
    CHECK_FALSE(sec::contains(root, child));

    child = sec::encode(9, 4, 4);
    CHECK_FALSE(sec::contains(root, child));
    child = sec::encode(9, 5, 5);
    CHECK_FALSE(sec::contains(root, child));

    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(0, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(1, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(2, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(3, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(4, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(5, 0, 0)));
    CHECK(sec::contains(sec::encode(6, 0, 0), sec::encode(6, 0, 0)));
}

TEST_CASE("DhSectionPosTest.containsAdjacentPosTest") {
    // neither should contain the other, they are single blocks that are next to each other
    Pos left  = sec::encode(0, 4606, 0);
    Pos right = sec::encode(0, 4607, 0);
    CHECK_FALSE(sec::contains(left, right));
    CHECK_FALSE(sec::contains(right, left));

    // 512 block wide sections that are adjacent, but not overlapping
    left  = sec::encode(9, 0, 0);
    right = sec::encode(9, 1, 0);
    CHECK_FALSE(sec::contains(left, right));
    CHECK_FALSE(sec::contains(right, left));
}

TEST_CASE("DhSectionPosTest.parentPosTest") {
    Pos leaf    = sec::encode(0, 0, 0);
    Pos convert = sec::convertToDetail(leaf, 1);
    Pos parent  = sec::parent(leaf);
    assertSectionPosEqual(convert, parent);

    leaf    = sec::encode(0, 1, 1);
    convert = sec::convertToDetail(leaf, 1);
    parent  = sec::parent(leaf);
    assertSectionPosEqual(convert, parent);

    leaf    = sec::encode(1, 2, 2);
    convert = sec::convertToDetail(leaf, 2);
    parent  = sec::parent(leaf);
    assertSectionPosEqual(convert, parent);
    convert        = sec::convertToDetail(leaf, 0);
    Pos childIndex = sec::child(leaf, 0);
    assertSectionPosEqual(convert, childIndex);
}

TEST_CASE("DhSectionPosTest.childPosTest") {
    Pos node = sec::encode(1, 2302, 0);
    Pos nw   = sec::child(node, 0);
    Pos sw   = sec::child(node, 1);
    Pos ne   = sec::child(node, 2);
    Pos se   = sec::child(node, 3);

    // confirm no children have the same values
    CHECK(nw != sw);
    CHECK(sw != ne);
    CHECK(ne != se);

    // confirm each child has the correct value
    assertSectionPosEqual(nw, sec::encode(0, 4604, 0));
    assertSectionPosEqual(sw, sec::encode(0, 4605, 0));
    assertSectionPosEqual(ne, sec::encode(0, 4604, 1));
    assertSectionPosEqual(se, sec::encode(0, 4605, 1));
}

TEST_CASE("DhSectionPosTest.getCenterBlock2DTest") {
    Pos parentNode = sec::encode(2, 1151, 0); // width 4 blocks
    Pos inputPos   = sec::encode(0, 4606, 0); // width 1 block
    CHECK(sec::contains(parentNode, inputPos));

    BlockPos2D parentCenter = sec::centerBlockPos(parentNode);
    BlockPos2D inputCenter  = sec::centerBlockPos(inputPos);

    CHECK(BlockPos2D(4606, 2) == parentCenter);
    CHECK(BlockPos2D(4606, 0) == inputCenter);
}

TEST_CASE("DhSectionPosTest.createFromBlockPos") {
    // origin pos
    BlockPos originBlockPos(0, 0, 0);
    Pos      originSectionPos = sec::encodeContaining(BLOCK_LVL, originBlockPos);
    assertSectionPosEqual(sec::encode(BLOCK_LVL, 0, 0), originSectionPos);

    // offset pos
    BlockPos offsetBlockPos(1000, 0, 42000);
    Pos      offsetSectionPos = sec::encodeContaining(BLOCK_LVL, offsetBlockPos);
    assertSectionPosEqual(sec::encode(BLOCK_LVL, 15, 656), offsetSectionPos);

    offsetBlockPos   = BlockPos(-987654, 0, 46);
    offsetSectionPos = sec::encodeContaining(BLOCK_LVL, offsetBlockPos);
    assertSectionPosEqual(sec::encode(BLOCK_LVL, -15433, 0), offsetSectionPos);
}

TEST_CASE("DhSectionPosTest.createFromChunkPos") {
    // origin pos
    ChunkPos originChunkPos(0, 0);
    Pos      originSectionPos = sec::encodeContaining(CHUNK_LVL, originChunkPos);
    assertSectionPosEqual(sec::encode(CHUNK_LVL, 0, 0), originSectionPos);

    // offset pos
    ChunkPos offsetChunkPos(1000, 42000);
    Pos      offsetSectionPos = sec::encodeContaining(CHUNK_LVL, offsetChunkPos);
    assertSectionPosEqual(sec::encode(CHUNK_LVL, 15, 656), offsetSectionPos);

    offsetChunkPos   = ChunkPos(-987654, 46);
    offsetSectionPos = sec::encodeContaining(CHUNK_LVL, offsetChunkPos);
    assertSectionPosEqual(sec::encode(CHUNK_LVL, -15433, 0), offsetSectionPos);
}

TEST_CASE("DhSectionPosTest.convertToDetailLevel") {
    // origin pos
    Pos originSectionPos = sec::encode(0, 0, 0);

    originSectionPos = sec::convertToDetail(originSectionPos, 1);
    assertSectionPosEqual(sec::encode(1, 0, 0), originSectionPos);

    originSectionPos = sec::convertToDetail(originSectionPos, BLOCK_LVL);
    assertSectionPosEqual(sec::encode(BLOCK_LVL, 0, 0), originSectionPos);

    originSectionPos = sec::convertToDetail(originSectionPos, REGION_LVL);
    assertSectionPosEqual(sec::encode(REGION_LVL, 0, 0), originSectionPos);

    // offset pos
    Pos offsetSectionPos = sec::encode(0, -10000, 5000);

    offsetSectionPos = sec::convertToDetail(offsetSectionPos, 1);
    assertSectionPosEqual(sec::encode(1, -5000, 2500), offsetSectionPos);

    offsetSectionPos = sec::convertToDetail(offsetSectionPos, BLOCK_LVL);
    assertSectionPosEqual(sec::encode(BLOCK_LVL, -157, 78), offsetSectionPos);

    offsetSectionPos = sec::convertToDetail(offsetSectionPos, REGION_LVL);
    assertSectionPosEqual(sec::encode(REGION_LVL, -1, 0), offsetSectionPos);
}

TEST_CASE("DhSectionPosTest.getOffsetWidth") {
    Pos originSectionPos = sec::encode(0, 0, 0);
    Pos sectionPos       = sec::encode(0, -10000, 5000);

    // 1 -> 0
    std::int8_t returnDetailLevel = 0;
    originSectionPos              = sec::convertToDetail(originSectionPos, 1);
    CHECK(sec::widthCountForLowerDetailedSection(originSectionPos, returnDetailLevel) == 2);
    sectionPos = sec::convertToDetail(sectionPos, 1);
    CHECK(sec::widthCountForLowerDetailedSection(sectionPos, returnDetailLevel) == 2);

    // 2 -> 1
    returnDetailLevel = 1;
    originSectionPos  = sec::convertToDetail(originSectionPos, 2);
    CHECK(sec::widthCountForLowerDetailedSection(originSectionPos, returnDetailLevel) == 2);
    sectionPos = sec::convertToDetail(sectionPos, 2);
    CHECK(sec::widthCountForLowerDetailedSection(sectionPos, returnDetailLevel) == 2);

    // Block -> 0
    returnDetailLevel = 0;
    originSectionPos  = sec::convertToDetail(originSectionPos, BLOCK_LVL);
    CHECK(sec::widthCountForLowerDetailedSection(originSectionPos, returnDetailLevel) == 64);
    sectionPos = sec::convertToDetail(sectionPos, BLOCK_LVL);
    CHECK(sec::widthCountForLowerDetailedSection(sectionPos, returnDetailLevel) == 64);

    // Region -> 3
    returnDetailLevel = 3;
    originSectionPos  = sec::convertToDetail(originSectionPos, REGION_LVL);
    CHECK(sec::widthCountForLowerDetailedSection(originSectionPos, returnDetailLevel) == 4096);
    sectionPos = sec::convertToDetail(sectionPos, REGION_LVL);
    CHECK(sec::widthCountForLowerDetailedSection(sectionPos, returnDetailLevel) == 4096);
}

TEST_CASE("DhSectionPosTest.getBlockWidth") {
    Pos originSectionPos = sec::encode(0, 0, 0);
    Pos sectionPos       = sec::encode(0, -10000, 5000);

    CHECK(sec::blockWidth(originSectionPos) == 1);
    CHECK(sec::blockWidth(sectionPos) == 1);

    originSectionPos = sec::convertToDetail(originSectionPos, 1);
    CHECK(sec::blockWidth(originSectionPos) == 2);
    sectionPos = sec::convertToDetail(sectionPos, 1);
    CHECK(sec::blockWidth(sectionPos) == 2);

    originSectionPos = sec::convertToDetail(originSectionPos, BLOCK_LVL);
    CHECK(sec::blockWidth(originSectionPos) == 64);
    sectionPos = sec::convertToDetail(sectionPos, BLOCK_LVL);
    CHECK(sec::blockWidth(sectionPos) == 64);

    originSectionPos = sec::convertToDetail(originSectionPos, REGION_LVL);
    CHECK(sec::blockWidth(originSectionPos) == 32768);
    sectionPos = sec::convertToDetail(sectionPos, REGION_LVL);
    CHECK(sec::blockWidth(sectionPos) == 32768);
}

TEST_CASE("DhSectionPosTest.getCenterBlockPosOrigin") {
    Pos originSectionPos = sec::encode(0, 0, 0);
    Pos sectionPos       = sec::encode(0, -10000, 5000);

    // 1x1 blocks
    CHECK(BlockPos2D(0, 0) == sec::centerBlockPos(originSectionPos));
    CHECK(BlockPos2D(-10000, 5000) == sec::centerBlockPos(sectionPos));

    // 2x2 blocks
    originSectionPos = sec::convertToDetail(originSectionPos, 1);
    CHECK(BlockPos2D(0, 0) == sec::centerBlockPos(originSectionPos));
    sectionPos = sec::convertToDetail(sectionPos, 1);
    CHECK(BlockPos2D(-10000, 5000) == sec::centerBlockPos(sectionPos));

    // 4x4 blocks
    originSectionPos = sec::convertToDetail(originSectionPos, 2);
    CHECK(BlockPos2D(2, 2) == sec::centerBlockPos(originSectionPos));
    sectionPos = sec::convertToDetail(sectionPos, 2);
    CHECK(BlockPos2D(-9998, 5002) == sec::centerBlockPos(sectionPos));

    // 64x64 blocks
    originSectionPos = sec::convertToDetail(originSectionPos, BLOCK_LVL);
    CHECK(BlockPos2D(32, 32) == sec::centerBlockPos(originSectionPos));
    sectionPos = sec::convertToDetail(sectionPos, BLOCK_LVL);
    CHECK(BlockPos2D(-10016, 5024) == sec::centerBlockPos(sectionPos));

    // 32,768 x 32,768 blocks
    originSectionPos = sec::convertToDetail(originSectionPos, REGION_LVL);
    CHECK(BlockPos2D(16384, 16384) == sec::centerBlockPos(originSectionPos));
    sectionPos = sec::convertToDetail(sectionPos, REGION_LVL);
    CHECK(BlockPos2D(-16384, 16384) == sec::centerBlockPos(sectionPos));
}

TEST_CASE("DhSectionPosTest.getMinCornerBlockPos") {
    Pos pos;

    // origin block detail
    pos = sec::encode(0, 0, 0);
    CHECK(sec::minCornerBlockX(pos) == 0);
    CHECK(sec::minCornerBlockZ(pos) == 0);

    // offset block detail
    pos = sec::encode(0, 2, 3);
    CHECK(sec::minCornerBlockX(pos) == 2 * sec::blockWidth(pos));
    CHECK(sec::minCornerBlockZ(pos) == 3 * sec::blockWidth(pos));

    // negative offset block detail
    pos = sec::encode(0, -1, -2);
    CHECK(sec::minCornerBlockX(pos) == -1 * sec::blockWidth(pos));
    CHECK(sec::minCornerBlockZ(pos) == -2 * sec::blockWidth(pos));

    // origin chunk detail
    pos = sec::encode(CHUNK_LVL, 0, 0);
    CHECK(sec::minCornerBlockX(pos) == 0);
    CHECK(sec::minCornerBlockZ(pos) == 0);

    // offset chunk detail
    pos = sec::encode(CHUNK_LVL, 2, 3);
    CHECK(sec::minCornerBlockX(pos) == 2 * sec::blockWidth(pos));
    CHECK(sec::minCornerBlockZ(pos) == 3 * sec::blockWidth(pos));
}

TEST_CASE("DhSectionPosTest.getAdjacentPos") {
    Pos pos = sec::encode(0, 0, 0);

    assertSectionPosEqual(sec::encode(0, 0, -1), sec::adjacent(pos, Direction::North));
    assertSectionPosEqual(sec::encode(0, 0, 1), sec::adjacent(pos, Direction::South));

    assertSectionPosEqual(sec::encode(0, 1, 0), sec::adjacent(pos, Direction::East));
    assertSectionPosEqual(sec::encode(0, -1, 0), sec::adjacent(pos, Direction::West));

    // getting the adjacent position in the up and down position don't make sense
    // (Java: assertThrows(IllegalArgumentException); C++ equivalent is nullopt)
    CHECK_FALSE(sec::tryAdjacent(pos, Direction::Up).has_value());
    CHECK_FALSE(sec::tryAdjacent(pos, Direction::Down).has_value());
}

TEST_CASE("DhSectionPosTest.forEachChildIterator") {
    Pos pos = sec::encode(1, 0, 0);

    std::vector<Pos> childPosList;
    int              childCount = 0;
    sec::forEachChild(pos, [&](Pos childPos) {
        childCount++;
        childPosList.push_back(childPos);
    });
    CHECK(childCount == 4);

    auto has = [&](Pos p) { return std::find(childPosList.begin(), childPosList.end(), p) != childPosList.end(); };
    CHECK(has(sec::encode(0, 0, 0)));
    CHECK(has(sec::encode(0, 1, 0)));
    CHECK(has(sec::encode(0, 0, 1)));
    CHECK(has(sec::encode(0, 1, 1)));
}

//=======================================//
// additional DhSectionPos spec-by-test  //
//=======================================//

TEST_CASE("sec::relativePosForDetail verbatim semantics") {
    // Hand-traced from DhSectionPos.getDhSectionRelativePositionForDetailLevel.
    auto rel = [](int d, int px, int pz, int out) {
        return sec::relativePosForDetail(
            sec::encode(static_cast<std::int8_t>(d), px, pz),
            static_cast<std::int8_t>(out)
        );
    };
    // same level, positives wrap modulo 64
    assertSectionPosEqual(sec::encode(6, 5, 9), rel(6, 5, 9, 6));
    assertSectionPosEqual(sec::encode(6, 0, 1), rel(6, 64, 65, 6));
    // same level negative: blockOffset = max(1, 0) = 1; -1 -> 63 + (0 % 64) = 63
    assertSectionPosEqual(sec::encode(6, 63, 63), rel(6, -1, -1, 6));
    // -64: xInput = -63; 63 + (-63 % 64) = 63 - 63 = 0
    assertSectionPosEqual(sec::encode(6, 0, 0), rel(6, -64, -64, 6));
    // -2: xInput = -1; 63 + (-1 % 64) = 62
    assertSectionPosEqual(sec::encode(6, 62, 62), rel(6, -2, -2, 6));
    // 2 levels up, positive: 5 / 4 = 1
    assertSectionPosEqual(sec::encode(8, 1, 2), rel(6, 5, 9, 8));
    // 2 levels up, negative: offset 3 -> (-5 + 3) / 4 = 0 -> 63 + 0
    assertSectionPosEqual(sec::encode(8, 63, 63), rel(6, -5, -5, 8));
    // 2 levels up, negative: (-13 + 3) / 4 = -10 / 4 = -2 (truncating) -> 63 + (-2) = 61
    assertSectionPosEqual(sec::encode(8, 61, 61), rel(6, -13, -13, 8));
    CHECK_FALSE(sec::tryRelativePosForDetail(sec::encode(6, 0, 0), 5).has_value());
}

TEST_CASE("sec: negative block/chunk to section uses truncating (p+1)/w - 1") {
    // block -1 -> section -1; block -64 -> (-63)/64 - 1 = -1; block -65 -> (-64)/64 - 1 = -2
    CHECK(sec::x(sec::encodeContainingBlock(6, -1, 0)) == -1);
    CHECK(sec::x(sec::encodeContainingBlock(6, -64, 0)) == -1);
    CHECK(sec::x(sec::encodeContainingBlock(6, -65, 0)) == -2);
    CHECK(sec::x(sec::encodeContainingBlock(6, 63, 0)) == 0);
    CHECK(sec::x(sec::encodeContainingBlock(6, 64, 0)) == 1);
    CHECK(sec::z(sec::encodeContainingChunk(6, 0, -4)) == -1);
    CHECK(sec::z(sec::encodeContainingChunk(6, 0, -5)) == -2);
}

TEST_CASE("sec: detail level 1 special case and Java int wrap") {
    // detail 1: centre has no half-offset, min corner has no half-width subtracted
    Pos p = sec::encode(1, 3, -2);
    CHECK(sec::centerBlockX(p) == 6);
    CHECK(sec::centerBlockZ(p) == -4);
    CHECK(sec::minCornerBlockX(p) == 6);
    CHECK(sec::minCornerBlockZ(p) == -4);

    // x * 2^detail overflows int in Java: x = 2^27 - 1, detail 5 -> wraps
    Pos big = sec::encode(5, (1 << 27) - 1, 0);
    CHECK(sec::centerBlockX(big) == static_cast<std::int32_t>((static_cast<std::uint32_t>((1 << 27) - 1) << 5) + 16));

    // shift counts are masked: powerOfTwo(32) == 1, powerOfTwo(31) == INT_MIN
    CHECK(sec::detailLevelWidthInBlocks(32) == 1);
    CHECK(sec::detailLevelWidthInBlocks(31) == INT32_MIN);
    CHECK(sec::detailLevelWidthInBlocks(127) == INT32_MIN);
}

TEST_CASE("sec: child / parent index conventions") {
    Pos node = sec::encode(3, -3, 5);
    for (int i = 0; i < 4; i++) {
        Pos c = sec::child(node, i);
        CHECK(sec::x(c) == -6 + (i & 1));
        CHECK(sec::z(c) == 10 + ((i & 2) >> 1));
        CHECK(sec::childIndexOfParent(c) == i);
        CHECK(sec::parent(c) == node);
    }
    // parent floors (arithmetic shift) for negatives
    CHECK(sec::x(sec::parent(sec::encode(0, -1, -2))) == -1);
    CHECK(sec::z(sec::parent(sec::encode(0, -1, -2))) == -1);
    CHECK(sec::tryChild(sec::encode(0, 0, 0), 0) == std::nullopt);
    CHECK(sec::tryChild(sec::encode(2, 0, 0), 4) == std::nullopt);
}

TEST_CASE("sec: distances and iterators") {
    Pos p = sec::encode(6, 0, 0); // centre (32, 32), width 64
    CHECK(sec::manhattanBlockDistance(p, 40, 30) == 10);
    CHECK(sec::chebyshevSignedDistance(p, 32, 32) == -32);
    CHECK(sec::chebyshevSignedDistance(p, 100, 32) == 36);
    CHECK(sec::chebyshevSignedDistance(p, BlockPos2D(100, 32)) == 36);

    std::vector<Pos> seen;
    sec::forEachChildAtDetailLevel(sec::encode(3, 0, 0), 1, [&](Pos c) { seen.push_back(c); });
    CHECK(seen.size() == 16);
    seen.clear();
    sec::forEachPosUpToDetailLevel(sec::encode(0, 5, 5), 3, [&](Pos c) { seen.push_back(c); });
    REQUIRE(seen.size() == 4);
    CHECK(sec::detail(seen[3]) == 3);
    seen.clear();
    sec::forEachChildDownToDetailLevel(sec::encode(2, 0, 0), 0, [&](Pos c) {
        seen.push_back(c);
        return false;
    });
    CHECK(seen.size() == 1 + 4 + 16);

    CHECK(sec::toString(sec::encode(6, 1, -3)) == "6*1,-3");
}

//==========//
// property //
//==========//

TEST_CASE("property: section pos encode/decode round trip") {
    std::mt19937_64                             rng(0xD157A27ull);
    std::uniform_int_distribution<std::int32_t> coord(-(1 << 27), (1 << 27) - 1);
    std::uniform_int_distribution<int>          det(0, 127);
    for (int i = 0; i < 200000; i++) {
        const std::int32_t px = coord(rng), pz = coord(rng);
        const std::int8_t  d = static_cast<std::int8_t>(det(rng));
        const Pos          p = sec::encode(d, px, pz);
        REQUIRE(sec::detail(p) == d);
        REQUIRE(sec::x(p) == px);
        REQUIRE(sec::z(p) == pz);
    }
    // out-of-range values are masked, then sign-extended from 28 bits
    CHECK(sec::x(sec::encode(0, 1 << 27, 0)) == -(1 << 27));
    CHECK(sec::x(sec::encode(0, (1 << 28), 0)) == 0);
    // negative detail levels keep only 7 bits (-1 -> 127)
    CHECK(sec::detail(sec::encode(-1, 0, 0)) == 127);
    CHECK(sec::detail(sec::encode(-128, 0, 0)) == 0);
}

TEST_CASE("property: children/parent/convert are consistent, no UB on extreme values") {
    std::mt19937_64 rng(42);
    // |coord| < 2^26 so that 2x+1 still fits the 28-bit field (children of edge positions wrap)
    std::uniform_int_distribution<std::int32_t> coord(-(1 << 26), (1 << 26) - 1);
    std::uniform_int_distribution<int>          det(1, 127);
    for (int i = 0; i < 100000; i++) {
        const Pos p = sec::encode(static_cast<std::int8_t>(det(rng)), coord(rng), coord(rng));
        for (int c = 0; c < 4; c++) {
            const Pos ch = sec::child(p, c);
            REQUIRE(sec::parent(ch) == p);
            REQUIRE(sec::childIndexOfParent(ch) == c);
        }
        // exercise wrapping paths (results unchecked; UBSan is the oracle)
        (void)sec::contains(p, sec::child(p, 3));
        (void)sec::minCornerBlockX(p);
        (void)sec::centerBlockZ(p);
        (void)sec::chebyshevSignedDistance(p, INT32_MIN, INT32_MAX);
        (void)sec::convertToDetail(p, static_cast<std::int8_t>(det(rng)));
        (void)sec::relativePosForDetail(sec::encode(0, coord(rng), coord(rng)), static_cast<std::int8_t>(det(rng)));
    }
}

//=====================//
// FullDataMinMaxPosTest //
//=====================//

TEST_CASE("FullDataMinMaxPosTest.EncodeAdjacentMinMaxPosTest") {
    const int maxTest = 3;
    for (std::int16_t minX = 0; minX < maxTest; minX++) {
        for (std::int16_t maxX = 0; maxX < maxTest; maxX++) {
            for (std::int16_t minZ = 0; minZ < maxTest; minZ++) {
                for (std::int16_t maxZ = 0; maxZ < maxTest; maxZ++) {
                    const std::int64_t encodedPos = minmax::encodeAdjMinMaxPos(minX, maxX, minZ, maxZ);
                    CHECK(minX == minmax::getAdjMinX(encodedPos));
                    CHECK(maxX == minmax::getAdjMaxX(encodedPos));
                    CHECK(minZ == minmax::getAdjMinZ(encodedPos));
                    CHECK(maxZ == minmax::getAdjMaxZ(encodedPos));
                }
            }
        }
    }
}

TEST_CASE("MinMaxPos: edge strips and sign-extension quirk") {
    auto n = minmax::getEncodedMinMaxPos(Direction::North);
    REQUIRE(n.has_value());
    CHECK(minmax::getAdjMinX(*n) == 0);
    CHECK(minmax::getAdjMaxX(*n) == 64);
    CHECK(minmax::getAdjMinZ(*n) == 0);
    CHECK(minmax::getAdjMaxZ(*n) == 1);
    auto s = minmax::getEncodedMinMaxPos(Direction::South);
    CHECK(minmax::getAdjMinZ(*s) == 63);
    CHECK(minmax::getAdjMaxZ(*s) == 64);
    auto e = minmax::getEncodedMinMaxPos(Direction::East);
    CHECK(minmax::getAdjMinX(*e) == 63);
    CHECK(minmax::getAdjMaxX(*e) == 64);
    auto w = minmax::getEncodedMinMaxPos(Direction::West);
    CHECK(minmax::getAdjMinX(*w) == 0);
    CHECK(minmax::getAdjMaxX(*w) == 1);
    CHECK_FALSE(minmax::getEncodedMinMaxPos(Direction::Up).has_value());
    CHECK_FALSE(minmax::getEncodedMinMaxPos(Direction::Down).has_value());

    // (long) of a negative short sign-extends and smears into the higher fields (Java behaviour)
    const std::int64_t e2 = minmax::encodeAdjMinMaxPos(-1, 0, 0, 0);
    CHECK(static_cast<std::uint64_t>(e2) == 0xFFFFFFFFFFFFFFFFull);
}

//==============//
// other pos types //
//==============//

TEST_CASE("Direction mirrors EDhDirection") {
    CHECK(static_cast<int>(Direction::Down) == 0);
    CHECK(static_cast<int>(Direction::Up) == 1);
    CHECK(static_cast<int>(Direction::North) == 2);
    CHECK(static_cast<int>(Direction::South) == 3);
    CHECK(static_cast<int>(Direction::West) == 4);
    CHECK(static_cast<int>(Direction::East) == 5);
    CHECK(dir::normal(Direction::North) == Int3{0, 0, -1});
    CHECK(dir::normal(Direction::East) == Int3{1, 0, 0});
    CHECK(dir::opposite(Direction::Up) == Direction::Down);
    CHECK(dir::opposite(Direction::West) == Direction::East);
    CHECK(dir::getAxis(Direction::South) == Axis::Z);
    CHECK(dir::compassIndex(Direction::East) == 3);
    CHECK(dir::compassIndex(Direction::Up) == -1);
    CHECK(dir::faceIndex(Direction::East) == 5);
    for (Direction d : dir::ALL) {
        CHECK(dir::opposite(dir::opposite(d)) == d);
    }
}

TEST_CASE("BlockPos / BlockPos2D") {
    BlockPos p(10, 64, -3);
    CHECK(p.createOffset(Direction::Up) == BlockPos(10, 65, -3));
    CHECK(p.createOffset(1, 2, 3) == BlockPos(11, 66, 0));
    CHECK(p.createOffset(INT32_MAX, 0, 0).x == INT32_MIN + 9); // wraps
    CHECK(BlockPos(17, 0, -1).createChunkRelativePos() == BlockPos(1, 0, 15));
    CHECK(BlockPos(-16, 5, -17).createChunkRelativePos() == BlockPos(0, 5, 15));
    CHECK(p.getManhattanDistance(BlockPos(0, 0, 0)) == 77);
    CHECK(BlockPos(1, 2, 3).hashCode() == (31 * (31 * (31 * 1 + 1) + 2) + 3));
    CHECK(p.toString() == "DHBlockPos[10, 64, -3]");

    BlockPos2D a(3, 4), b(0, 0);
    CHECK(a.dist(b) == doctest::Approx(5.0));
    CHECK(a.distSquared(b) == 25);
    CHECK(a.chebyshevDist(b) == 4);
    CHECK(a.manhattanDist(b) == 7);
    CHECK(a.add(1, 1) == BlockPos2D(4, 5));
    CHECK(a.subtract(BlockPos2D(1, 1)) == BlockPos2D(2, 3));
    CHECK(a.hashCode() == (3 ^ 4));
    CHECK(a.toString() == "(3, 4)");
    CHECK(BlockPos2D(INT32_MIN, 0).distSquared(0, 0) == (static_cast<std::int64_t>(INT32_MIN) * INT32_MIN));
}

TEST_CASE("ChunkPos") {
    CHECK(ChunkPos(BlockPos(-1, 0, 16)) == ChunkPos(-1, 1));
    CHECK(ChunkPos(BlockPos2D(-17, 15)) == ChunkPos(-2, 0));
    // (int) truncation toward zero happens BEFORE the shift: -0.5 -> 0 -> chunk 0 (not -1)
    CHECK(ChunkPos::fromWorldPos(-0.5, 17.9) == ChunkPos(0, 1));
    CHECK(ChunkPos::fromWorldPos(-16.5, 0.0) == ChunkPos(-1, 0)); // (int) -16.5 == -16 -> -1
    CHECK(ChunkPos::fromWorldPos(-17.5, 0.0) == ChunkPos(-2, 0));

    ChunkPos c(2, -3);
    CHECK(c.getMinBlockX() == 32);
    CHECK(c.getMinBlockZ() == -48);
    CHECK(c.minCornerBlockPos() == BlockPos(32, 0, -48));
    // centerBlockPos is (8 + x) << 4 in Java (operator precedence) = 160, NOT 40
    CHECK(c.centerBlockPos() == BlockPos(160, 0, 80));
    CHECK(c.getMaxBlockX() == 48);
    CHECK(c.getMaxBlockZ() == -33);              // -48 + 16 = -32 < 0 -> -33
    CHECK(ChunkPos(-1, -1).getMaxBlockX() == 0); // -16 + 16 = 0, not negative
    CHECK(c.contains(BlockPos(32, 5, -48)));
    CHECK(c.contains(BlockPos(47, 5, -33)));
    CHECK_FALSE(c.contains(BlockPos(48, 5, -33)));
    CHECK(c.contains(BlockPos2D(40, -40)));
    CHECK(c.hashCode() == 2 * 7309 - 3);
    CHECK(c.distance(ChunkPos(5, 1)) == doctest::Approx(5.0));
    CHECK(c.squaredDistance(ChunkPos(5, 1)) == doctest::Approx(25.0));
    CHECK(c.toString() == "C[2,-3]");
}
