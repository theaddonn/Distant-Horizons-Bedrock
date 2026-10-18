// Golden vectors for DhSectionPos (tests/golden/pos.gv.json).
//
// SKIPPED_KEYS (no recoverable C++ variant; the C++ port assert()s instead of rejecting):
//   getWidthCountForLowerDetailedSection  - Java AssertFailureException when returnDetail > detail
//                                           (only the cases where Java returns a value are checked).
#include <doctest.h>

#include "dh/pos/SectionPos.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

TEST_CASE("golden pos: DhSectionPos") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "pos");
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);

        const int detail = c.inI("detail");
        c.eq("encode", sec::encode(static_cast<std::int8_t>(detail), c.inI("x"), c.inI("z")));

        // Everything else is derived from Java's encoded value so a bad encode does not cascade.
        const sec::Pos pos = c.outHex("encode");

        c.eq("getDetailLevel", sec::detail(pos));
        c.eq("getX", sec::x(pos));
        c.eq("getZ", sec::z(pos));
        c.eq("getParentPos", sec::parent(pos));

        c.eqOpt("getChildByIndex_0", sec::tryChild(pos, 0));
        c.eqOpt("getChildByIndex_1", sec::tryChild(pos, 1));
        c.eqOpt("getChildByIndex_2", sec::tryChild(pos, 2));
        c.eqOpt("getChildByIndex_3", sec::tryChild(pos, 3));
        c.eq("getChildIndexOfParent", sec::childIndexOfParent(pos));

        c.eqOpt("getAdjacentPos_DOWN", sec::tryAdjacent(pos, Direction::Down));
        c.eqOpt("getAdjacentPos_UP", sec::tryAdjacent(pos, Direction::Up));
        c.eqOpt("getAdjacentPos_NORTH", sec::tryAdjacent(pos, Direction::North));
        c.eqOpt("getAdjacentPos_SOUTH", sec::tryAdjacent(pos, Direction::South));
        c.eqOpt("getAdjacentPos_WEST", sec::tryAdjacent(pos, Direction::West));
        c.eqOpt("getAdjacentPos_EAST", sec::tryAdjacent(pos, Direction::East));

        c.eq("convertToDetailLevel", sec::convertToDetail(pos, static_cast<std::int8_t>(c.inI("targetDetail"))));
        c.eq("getMinCornerBlockX", sec::minCornerBlockX(pos));
        c.eq("getMinCornerBlockZ", sec::minCornerBlockZ(pos));
        c.eq("getCenterBlockPosX", sec::centerBlockX(pos));
        c.eq("getCenterBlockPosZ", sec::centerBlockZ(pos));
        c.eq("getBlockWidth", sec::blockWidth(pos));
        c.eq("getChunkWidth", sec::chunkWidth(pos));

        if (c.live("getWidthCountForLowerDetailedSection")) {
            c.eq(
                "getWidthCountForLowerDetailedSection",
                sec::widthCountForLowerDetailedSection(pos, static_cast<std::int8_t>(c.inI("returnDetail")))
            );
        }

        const BlockPos blk(c.inI("blockX"), c.inI("blockY"), c.inI("blockZ"));
        c.eq("contains_pos", sec::contains(pos, c.inHex("other")));
        c.eq("contains_block", sec::contains(pos, blk));

        c.eqOpt(
            "getDhSectionRelativePositionForDetailLevel",
            sec::tryRelativePosForDetail(pos, static_cast<std::int8_t>(c.inI("relDetail")))
        );
        c.eq("getChebyshevSignedBlockDistance", sec::chebyshevSignedDistance(pos, blk.x, blk.z));
        c.eq("getManhattanBlockDistance", sec::manhattanBlockDistance(pos, BlockPos2D(blk.x, blk.z)));

        const auto outDetail = static_cast<std::int8_t>(c.inI("outDetail"));
        c.eq("encodeContaining_block", sec::encodeContaining(outDetail, blk));
        c.eq("encodeContaining_chunk", sec::encodeContaining(outDetail, ChunkPos(c.inI("chunkX"), c.inI("chunkZ"))));
    }
    gv::report("pos", st);
}
