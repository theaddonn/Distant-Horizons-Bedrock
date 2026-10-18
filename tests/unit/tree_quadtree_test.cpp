#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dh/pos/BlockPos.h"
#include "dh/pos/SectionPos.h"
#include "dh/tree/ArrayGridList.h"
#include "dh/tree/GridRing.h"
#include "dh/tree/PosArrayGridList.h"
#include "dh/tree/QuadNode.h"
#include "dh/tree/QuadTree.h"
#include "dh/util/Bits.h"
#include "dh/util/MathUtil.h"

// Port of core/src/test/java/tests/QuadTreeTest.java (17 tests, same literals) plus ring-list and
// iteration-order tests. Java's IndexOutOfBoundsException expectation maps to "setValue/getValue return false".

using namespace dh;
using namespace dh::tree;
using sec::Pos;

namespace {

using IntTree = QuadTree<int>;
using IntNode = QuadNode<int>;
using Opt     = std::optional<int>;

constexpr std::int8_t BLOCK_DETAIL_LEVEL = lod::BLOCK_DETAIL_LEVEL; // LodUtil.BLOCK_DETAIL_LEVEL

//================//
// helper methods //
//================//

// testGet(tree, pos, value, expectedExceptionClass)
void testGetImpl(IntTree& tree, Pos pos, Opt getValue, bool expectOutOfBounds) {
    INFO("pos [" << sec::toString(pos) << "]");
    Opt  result;
    bool inBounds = tree.getValue(pos, result);
    if (expectOutOfBounds) {
        CHECK_FALSE(inBounds);
    } else {
        REQUIRE(inBounds);
        CHECK(getValue == result);
    }
}

// testSet(tree, pos, value, expectedExceptionClass): set, then get (confirm value was correctly set)
void testSet(IntTree& tree, Pos pos, Opt setValue, bool expectOutOfBounds = false) {
    INFO("pos [" << sec::toString(pos) << "]");
    const bool ok = tree.setValue(pos, setValue);
    if (expectOutOfBounds) {
        CHECK_FALSE(ok);
    } else {
        CHECK(ok);
    }
    testGetImpl(tree, pos, setValue, expectOutOfBounds);
}

// Java quirk kept: the 3 argument testGet() forwards to testSet().
void testGet(IntTree& tree, Pos pos, Opt getValue) { testSet(tree, pos, getValue, false); }
void testGet(IntTree& tree, Pos pos, Opt getValue, bool expectOutOfBounds) {
    testGetImpl(tree, pos, getValue, expectOutOfBounds);
}

struct AbstractTestTreeParams {
    virtual ~AbstractTestTreeParams()                            = default;
    virtual std::int32_t getWidthInBlocks() const                = 0;
    virtual std::int32_t getBlockDistanceForNodeClearing() const = 0;

    /// the tree should be slightly larger than the width in blocks to account for offset centers
    std::int32_t getWidthInRootNodes() const { return math::log2(getWidthInBlocks()) + 2; }
    /// the top (root) detail level in the tree
    std::int8_t getRootDetailLevel() const { return static_cast<std::int8_t>(math::log2(getWidthInBlocks())); }
    /// the block pos so that the tree's negative corner lines up with (0,0)
    BlockPos2D getPositiveEdgeCenterPos() const {
        return BlockPos2D(powerOfTwo(getRootDetailLevel()) / 2, powerOfTwo(getRootDetailLevel()) / 2);
    }
};
struct LargeTestTree : AbstractTestTreeParams {
    std::int32_t getWidthInBlocks() const override { return 8192; }
    std::int32_t getBlockDistanceForNodeClearing() const override { return 16; }
};
struct MediumTestTree : AbstractTestTreeParams {
    std::int32_t getWidthInBlocks() const override { return 1024; }
    std::int32_t getBlockDistanceForNodeClearing() const override { return 4; }
};
struct TinyTestTree : AbstractTestTreeParams { // top detail level = 6
    std::int32_t getWidthInBlocks() const override { return 32; }
    std::int32_t getBlockDistanceForNodeClearing() const override { return 1; }
};

template <class P>
std::unique_ptr<IntTree> makeTree(const P& p, BlockPos2D center, std::int8_t leaf) {
    return std::make_unique<IntTree>(p.getWidthInBlocks(), p.getBlockDistanceForNodeClearing(), center, leaf);
}

int countRootNodes(IntTree& tree) {
    int  rootNodeCount = 0;
    auto it            = tree.rootNodePosIterator();
    while (it.hasNext()) {
        IntNode* rootNode = tree.getNode(it.nextLong());
        if (rootNode != nullptr) {
            rootNodeCount++;
        }
    }
    return rootNodeCount;
}

} // namespace

//=========================//
// QuadTreeTest.java ports //
//=========================//

TEST_CASE("QuadTreeTest.BasicPositiveQuadTreeTest") {
    LargeTestTree p;
    auto          treeP = makeTree(p, p.getPositiveEdgeCenterPos(), BLOCK_DETAIL_LEVEL);
    IntTree&      tree  = *treeP;
    CHECK_MESSAGE(
        (tree.treeRootDetailLevel >= 10 && tree.treeLeafDetailLevel <= 10 - 4),
        "Tree min/max detail level out of expected bounds: ",
        tree.toString()
    );

    // (pseudo) root node //
    testSet(tree, sec::encode(10, 0, 0), 0);

    // first child (0,0) //
    testSet(tree, sec::encode(9, 0, 0), 1);
    testSet(tree, sec::encode(9, 1, 0), 2);
    testSet(tree, sec::encode(9, 0, 1), 3);
    testSet(tree, sec::encode(9, 1, 1), 4);

    // second child (0,0) (0,0) //
    testSet(tree, sec::encode(8, 0, 0), 5);
    testSet(tree, sec::encode(8, 1, 0), 6);
    testSet(tree, sec::encode(8, 0, 1), 7);
    testSet(tree, sec::encode(8, 1, 1), 8);
    // second child (0,0) (1,1) //
    testSet(tree, sec::encode(8, 2, 2), 9);
    testSet(tree, sec::encode(8, 3, 2), 10);
    testSet(tree, sec::encode(8, 2, 3), 11);
    testSet(tree, sec::encode(8, 3, 3), 12);

    // third child (0,0) (1,0) (0,0) //
    testSet(tree, sec::encode(7, 5, 0), 9);
    testSet(tree, sec::encode(7, 6, 0), 10);
    testSet(tree, sec::encode(7, 5, 1), 11);
    testSet(tree, sec::encode(7, 6, 1), 12);
}

TEST_CASE("QuadTreeTest.BasicNegativeQuadTreeTest") {
    LargeTestTree p;
    auto          treeP = makeTree(p, BLOCK_POS2D_ZERO, BLOCK_DETAIL_LEVEL);
    IntTree&      tree  = *treeP;

    // root node //
    testSet(tree, sec::encode(10, -1, -1), 0);

    // first child (-1,-1) //
    testSet(tree, sec::encode(9, -2, -1), 1);
    testSet(tree, sec::encode(9, -1, -1), 2);
    testSet(tree, sec::encode(9, -2, -2), 3);
    testSet(tree, sec::encode(9, -1, -2), 4);

    // second child (-1,-1) (0,0) //
    testSet(tree, sec::encode(8, 0, 0), 5);
    testSet(tree, sec::encode(8, 1, 0), 6);
    testSet(tree, sec::encode(8, 0, 1), 7);
    testSet(tree, sec::encode(8, 1, 1), 8);
    // second child (-1,-1) (1,1) //
    testSet(tree, sec::encode(8, 2, 2), 9);
    testSet(tree, sec::encode(8, 3, 2), 10);
    testSet(tree, sec::encode(8, 2, 3), 11);
    testSet(tree, sec::encode(8, 3, 3), 12);

    // third child (-1,-1) (1,0) (0,0) //
    testSet(tree, sec::encode(7, 5, 0), 9);
    testSet(tree, sec::encode(7, 6, 0), 10);
    testSet(tree, sec::encode(7, 5, 1), 11);
    testSet(tree, sec::encode(7, 6, 1), 12);
}

TEST_CASE("QuadTreeTest.OutOfBoundsQuadTreeTest") {
    LargeTestTree p;
    auto          treeP = makeTree(p, BlockPos2D(0, 0), BLOCK_DETAIL_LEVEL);
    IntTree&      tree  = *treeP;
    CHECK_MESSAGE(p.getWidthInBlocks() == tree.diameterInBlocks(), "tree diameter incorrect");

    // wrong detail level on purpose, if the detail level was 0 (block) this should work
    Pos outOfBoundsPos = sec::encode(sec::SECTION_BLOCK_DETAIL_LEVEL, p.getWidthInBlocks() / 2, 0);
    testSet(tree, outOfBoundsPos, -1, true);
    CHECK_MESSAGE(0 == tree.leafNodeCount(), "incorrect leaf node count");

    // out of bounds //
    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, (p.getWidthInBlocks() / 2) + 1, 0);
    testSet(tree, outOfBoundsPos, -1, true);
    CHECK_MESSAGE(0 == tree.leafNodeCount(), "incorrect leaf node count");

    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, p.getWidthInBlocks() / 2, 0);
    testSet(tree, outOfBoundsPos, -1, true);
    CHECK_MESSAGE(0 == tree.leafNodeCount(), "incorrect leaf node count");

    // in bounds //
    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, (p.getWidthInBlocks() / 2) - 1, 0);
    testSet(tree, outOfBoundsPos, 0);
    CHECK_MESSAGE(1 == tree.leafNodeCount(), "incorrect leaf node count");

    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, (p.getWidthInBlocks() / 2) - 3, 0);
    testSet(tree, outOfBoundsPos, 0);
    CHECK_MESSAGE(2 == tree.leafNodeCount(), "incorrect leaf node count");

    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, (p.getWidthInBlocks() / 2) - 2, 0);
    testSet(tree, outOfBoundsPos, 0);
    CHECK_MESSAGE(3 == tree.leafNodeCount(), "incorrect leaf node count");

    outOfBoundsPos = sec::encode(BLOCK_DETAIL_LEVEL, (p.getWidthInBlocks() / 2) - 4, 0);
    testSet(tree, outOfBoundsPos, 0);
    CHECK_MESSAGE(4 == tree.leafNodeCount(), "incorrect leaf node count");
}

TEST_CASE("QuadTreeTest.outOfBoundsInTreeTest") {
    // very specific tree parameters to match test results
    IntTree tree(512, 8, BlockPos2D(125, -516), 6);
    CHECK_MESSAGE(9 == tree.treeRootDetailLevel, "Test may need to be re-calculated for different max detail level.");

    Pos rootPos = sec::encode(9, 0, -1);
    testSet(tree, rootPos, 1);

    // pos is in tree, but out of range
    Pos midPos = sec::encode(8, 0, -1);
    testSet(tree, midPos, 2, true);

    // pos is in tree, but out of range
    Pos leafPos = sec::encode(7, 0, -2);
    testSet(tree, leafPos, 3, true);
}

TEST_CASE("QuadTreeTest.QuadTreeRootAlignedMovingTest") {
    LargeTestTree p;
    auto          treeP = makeTree(p, p.getPositiveEdgeCenterPos(), BLOCK_DETAIL_LEVEL);
    IntTree&      tree  = *treeP;

    int pseudoRootNodeWidthInBlocks = powerOfTwo(10);

    // (pseudo) root nodes //
    testSet(tree, sec::encode(10, 0, 0), 1);

    // first child (0,0) //
    Pos nw = sec::encode(sec::SECTION_BLOCK_DETAIL_LEVEL, 0, 0);
    Pos ne = sec::encode(sec::SECTION_BLOCK_DETAIL_LEVEL, 1, 0);
    Pos sw = sec::encode(sec::SECTION_BLOCK_DETAIL_LEVEL, 0, 1);
    Pos se = sec::encode(sec::SECTION_BLOCK_DETAIL_LEVEL, 1, 1);

    testSet(tree, nw, 2);
    testSet(tree, ne, 3);
    testSet(tree, sw, 4);
    testSet(tree, se, 5);
    CHECK_MESSAGE(4 == tree.leafNodeCount(), "incorrect leaf node count");

    // fake move //
    tree.setCenterBlockPos(BLOCK_POS2D_ZERO);
    CHECK_MESSAGE(BLOCK_POS2D_ZERO == tree.getCenterBlockPos(), "Tree center incorrect");

    testGet(tree, nw, 2);
    testGet(tree, ne, 3);
    testGet(tree, sw, 4);
    testGet(tree, se, 5);
    CHECK_MESSAGE(tree.leafNodeCount() == 4, "incorrect leaf node count");

    // small move //

    // move far enough that the original root nodes aren't touching the same grid
    // squares they were before, but not far enough as to be garbage collected
    BlockPos2D smallMoveBlockPos(pseudoRootNodeWidthInBlocks * 2, 0);
    tree.setCenterBlockPos(smallMoveBlockPos);
    CHECK_MESSAGE(smallMoveBlockPos == tree.getCenterBlockPos(), "Tree center incorrect");

    // nodes should be found at the same locations
    testGet(tree, nw, 2);
    testGet(tree, ne, 3);
    testGet(tree, sw, 4);
    testGet(tree, se, 5);
    CHECK_MESSAGE(4 == tree.leafNodeCount(), "incorrect leaf node count");

    // very large move //
    BlockPos2D bigMoveBlockPos(p.getWidthInBlocks() * 2, 0);
    tree.setCenterBlockPos(bigMoveBlockPos);
    CHECK_MESSAGE(bigMoveBlockPos == tree.getCenterBlockPos(), "Tree center incorrect");

    // nothing should be found in the tree
    testGet(tree, nw, std::nullopt, true);
    testGet(tree, ne, std::nullopt, true);
    testGet(tree, sw, std::nullopt, true);
    testGet(tree, se, std::nullopt, true);

    CHECK_MESSAGE(0 == tree.leafNodeCount(), "incorrect leaf node count");

    // edge move //

    // move back to the origin for easy testing
    tree.setCenterBlockPos(BLOCK_POS2D_ZERO);
    CHECK_MESSAGE(BLOCK_POS2D_ZERO == tree.getCenterBlockPos(), "Tree center incorrect");

    // on the negative X edge
    Pos edgePos = sec::encode(BLOCK_DETAIL_LEVEL, -p.getWidthInBlocks() / 2, 0);
    testSet(tree, edgePos, 1);
    CHECK_MESSAGE(1 == tree.leafNodeCount(), "incorrect leaf node count");

    // +1 root node from the negative X edge
    Pos adjacentEdgePos = sec::encode(BLOCK_DETAIL_LEVEL, (-p.getWidthInBlocks() / 2) + pseudoRootNodeWidthInBlocks, 0);
    testSet(tree, adjacentEdgePos, 2);
    CHECK_MESSAGE(2 == tree.leafNodeCount(), "incorrect leaf node count");

    // move so only the root nodes exactly on the X edge remain
    BlockPos2D edgeMoveBlockPos(
        pseudoRootNodeWidthInBlocks - (p.getWidthInRootNodes() * pseudoRootNodeWidthInBlocks),
        0
    );
    tree.setCenterBlockPos(edgeMoveBlockPos);
    CHECK_MESSAGE(edgeMoveBlockPos == tree.getCenterBlockPos(), "Tree center incorrect");
    CHECK_MESSAGE(0 == tree.leafNodeCount(), "incorrect leaf node count");
}

TEST_CASE("QuadTreeTest.QuadTreeIterationTest") {
    LargeTestTree p;
    auto          treeP = makeTree(p, p.getPositiveEdgeCenterPos(), BLOCK_DETAIL_LEVEL);
    IntTree&      tree  = *treeP;

    // (pseudo) root nodes //
    testSet(tree, sec::encode(10, 0, 0), 1);
    testSet(tree, sec::encode(10, 1, 0), 2);

    // first child (0,0) //
    testSet(tree, sec::encode(9, 0, 0), 3);
    testSet(tree, sec::encode(9, 1, 0), 4);
    testSet(tree, sec::encode(9, 0, 1), 5);
    testSet(tree, sec::encode(9, 1, 1), 6);

    // root nodes
    CHECK_MESSAGE(1 == countRootNodes(tree), "incorrect root count");

    // leaf nodes
    int  leafCount        = 0;
    int  leafValueSum     = 0;
    auto leafNodeIterator = tree.leafNodeIterator();
    while (leafNodeIterator.hasNext()) {
        IntNode* leafNode = leafNodeIterator.next();
        leafCount++;
        leafValueSum += *leafNode->value;
    }
    CHECK_MESSAGE(5 == leafCount, "incorrect leaf count");
    CHECK_MESSAGE(20 == leafValueSum, "incorrect leaf value sum");
}

namespace {
void recursivelyFillRootNodeTree(IntTree& tree, std::int8_t bottomDetailLevel, Pos rootNodePos) {
    std::int8_t thisDetailLevel = sec::detail(rootNodePos);
    tree.setValue(rootNodePos, static_cast<int>(thisDetailLevel));

    if (thisDetailLevel <= bottomDetailLevel) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        Pos childPos = sec::child(rootNodePos, i);
        recursivelyFillRootNodeTree(tree, bottomDetailLevel, childPos);
    }
}

void assertFilterCount(
    IntTree&                     tree,
    const std::string&           message,
    int                          expectedNodeCount,
    const NodeStoppingFunc<int>& stoppingFilterFunc
) {
    std::vector<std::string> foundNodePositionStrings;
    int                      filteredNodeCount = 0;
    auto                     it                = tree.nodeIteratorWithStoppingFilter(stoppingFilterFunc);
    while (it.hasNext()) {
        IntNode* node = it.next();
        foundNodePositionStrings.push_back(sec::toString(node->sectionPos));
        filteredNodeCount++;
    }
    INFO(message << " | found count: [" << foundNodePositionStrings.size() << "]");
    CHECK(expectedNodeCount == filteredNodeCount);
}
} // namespace

TEST_CASE("QuadTreeTest.QuadTreeIterationFilterTest") {
    TinyTestTree p;
    auto         treeP = makeTree(p, p.getPositiveEdgeCenterPos(), 0);
    IntTree&     tree  = *treeP;

    // fill tree root node(s)
    int  rootNodeCount       = 0;
    auto rootNodePosIterator = tree.rootNodePosIterator();
    while (rootNodePosIterator.hasNext()) {
        Pos rootNodePos = rootNodePosIterator.nextLong();
        recursivelyFillRootNodeTree(tree, 0, rootNodePos);
        rootNodeCount++;
    }
    CHECK_MESSAGE(1 == rootNodeCount, "Only one root node expected");

    // confirm tree isn't empty
    int totalNodeCount = tree.nodeCount();
    CHECK_MESSAGE(0 != totalNodeCount, "No nodes");
    int leafCount = tree.leafNodeCount();
    CHECK_MESSAGE(0 != leafCount, "No leaf nodes");

    // confirm tree has correct number of nodes
    int treeDepth = tree.treeRootDetailLevel - tree.treeLeafDetailLevel;

    // equation source: http://www.gamedev.net/forums/topic/325071-calculate-number-of-nodes-in-a-quad-tree/3098433/
    int expectedNodeCount = static_cast<int>(std::pow(4, treeDepth + 1));
    expectedNodeCount     = (expectedNodeCount - 1) / 3;

    CHECK_MESSAGE(expectedNodeCount == totalNodeCount, "Unexpected total node count");
    int expectedLeafNodeCount = static_cast<int>(std::pow(4, treeDepth));
    CHECK_MESSAGE(expectedLeafNodeCount == leafCount, "Unexpected leaf node count");

    // filters //

    // accept everything
    assertFilterCount(tree, "Get everything should return total node count", totalNodeCount, [](IntNode*) {
        return false;
    });
    // ignore everything
    assertFilterCount(tree, "Ignore everything filter shouldn't return anything", 0, [](IntNode*) { return true; });

    // root detail level
    assertFilterCount(tree, "filter root detail", 1, [&](IntNode* node) {
        if (node == nullptr) {
            return true;
        }
        return sec::detail(node->sectionPos) < tree.treeRootDetailLevel;
    });

    // root - 1 detail level
    assertFilterCount(tree, "filter root-1 detail", 1 + 4, [&](IntNode* node) {
        if (node == nullptr) {
            return true;
        }
        return sec::detail(node->sectionPos) < tree.treeRootDetailLevel - 1;
    });

    // only nodes in (4*0,0)
    assertFilterCount(
        tree,
        "filter to root-1 NW corner, should return 1/4th of all nodes plus root ",
        (totalNodeCount / 4) + 1,
        [](IntNode* node) {
            if (node == nullptr) {
                return true;
            }
            return !sec::contains(sec::encode(4, 0, 0), node->sectionPos);
        }
    );
}

TEST_CASE("QuadTreeTest.NewQuadTreeIterationTest") {
    IntNode rootNode(sec::encode(10, 0, 0), BLOCK_DETAIL_LEVEL);

    rootNode.setValue(sec::encode(10, 0, 0), 0);

    rootNode.setValue(sec::encode(9, 0, 0), 1);
    rootNode.setValue(sec::encode(9, 1, 0), 1);
    rootNode.setValue(sec::encode(9, 0, 1), 1);
    rootNode.setValue(sec::encode(9, 1, 1), std::nullopt);

    rootNode.setValue(sec::encode(8, 0, 0), 2);
    rootNode.setValue(sec::encode(8, 1, 0), 2);
    rootNode.setValue(sec::encode(8, 0, 1), 2);
    rootNode.setValue(sec::encode(8, 1, 1), std::nullopt);

    // all node iterator //
    auto iterator = rootNode.getNodeIterator();

    std::set<IntNode*> iteratedNodes;
    int                populatedValueCount = 0;
    int                totalNodeCount      = 0;
    while (iterator.hasNext()) {
        IntNode* node = iterator.next();
        if (node->value.has_value()) {
            populatedValueCount++;
        }
        if (!iteratedNodes.insert(node).second) {
            FAIL("Iterator passed over the same node multiple times. Section Pos: " << node->sectionPos);
        }
        totalNodeCount++;
    }
    CHECK_MESSAGE(7 == populatedValueCount, "incorrect populated node count");
    CHECK_MESSAGE(9 == totalNodeCount, "incorrect total node count");

    // leaf node iterator //
    auto leafIterator = rootNode.getLeafNodeIterator();

    std::set<IntNode*> iteratedLeafNodes;
    int                populatedLeafCount = 0;
    int                totalLeafCount     = 0;
    while (leafIterator.hasNext()) {
        IntNode* node = leafIterator.next();
        if (node->value.has_value()) {
            populatedLeafCount++;
        }
        if (!iteratedLeafNodes.insert(node).second) {
            FAIL("Iterator passed over the same node multiple times. Section Pos: " << node->sectionPos);
        }
        totalLeafCount++;
    }
    CHECK_MESSAGE(5 == populatedLeafCount, "incorrect populated leaf count");
    CHECK_MESSAGE(7 == totalLeafCount, "incorrect total leaf count");
}

TEST_CASE("QuadTreeTest.CenteredGridListIterationTest") {
    TinyTestTree p;
    auto         treeP = makeTree(p, p.getPositiveEdgeCenterPos(), BLOCK_DETAIL_LEVEL);
    IntTree&     tree  = *treeP;
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 0, 0), 0);

    // confirm the root node were added
    CHECK_MESSAGE(1 == countRootNodes(tree), "incorrect root count");

    // attempt to get and remove, each node in the tree
    int  rootNodePosCount    = 0;
    auto rootNodePosIterator = tree.rootNodePosIterator();
    while (rootNodePosIterator.hasNext()) {
        Pos      rootNodePos = rootNodePosIterator.nextLong();
        IntNode* rootNode    = tree.getNode(rootNodePos);
        if (rootNode != nullptr) {
            testGet(tree, rootNodePos, 0);
            testSet(tree, rootNodePos, std::nullopt);
            rootNodePosCount++;
        }
    }
    CHECK_MESSAGE(1 == rootNodePosCount, "incorrect root count");
}

namespace {
void testGridListRootCount(
    int        treeWidth,
    int        treeDistanceForNodeClearing,
    BlockPos2D treeMovePos,
    int        expectedRootNodeCount
) {
    IntTree tree(treeWidth, treeDistanceForNodeClearing, BLOCK_POS2D_ZERO, BLOCK_DETAIL_LEVEL);
    CHECK_MESSAGE(BLOCK_POS2D_ZERO == tree.getCenterBlockPos(), "tree creation failed, incorrect initial position");

    tree.setCenterBlockPos(treeMovePos);
    CHECK_MESSAGE(treeMovePos == tree.getCenterBlockPos(), "tree move failed, incorrect position after move");

    auto rootNodePosIterator = tree.rootNodePosIterator();
    while (rootNodePosIterator.hasNext()) {
        testSet(tree, rootNodePosIterator.nextLong(), 0);
    }

    // 4 root nodes should be added
    CHECK_MESSAGE(expectedRootNodeCount == countRootNodes(tree), "incorrect root count");
}
} // namespace

TEST_CASE("QuadTreeTest.OffsetGridListIterationTest") {
    TinyTestTree p;

    // exactly inside (5*0,0)
    testGridListRootCount(p.getWidthInBlocks(), p.getBlockDistanceForNodeClearing(), p.getPositiveEdgeCenterPos(), 1);

    // offset across (5*-1,0) and (5*0,0)
    testGridListRootCount(
        p.getWidthInBlocks(),
        p.getBlockDistanceForNodeClearing(),
        BlockPos2D(-p.getWidthInBlocks() / 4, p.getPositiveEdgeCenterPos().z),
        2
    );

    // offset across the origin: (5*0,0), (5*-1,0), (5*0,-1), and (5*-1,-1)
    testGridListRootCount(p.getWidthInBlocks(), p.getBlockDistanceForNodeClearing(), BLOCK_POS2D_ZERO, 4);
}

TEST_CASE("QuadTreeTest.TinyGridAlignedTreeTest") {
    MediumTestTree p;
    auto           treeP = makeTree(p, p.getPositiveEdgeCenterPos(), BLOCK_DETAIL_LEVEL);
    IntTree&       tree  = *treeP;
    // minimum size tree should be 3 root nodes wide
    CHECK_MESSAGE(3 == tree.ringListWidth(), "incorrect tree node width");
    CHECK_MESSAGE(p.getWidthInBlocks() == tree.diameterInBlocks(), "incorrect tree width");

    testSet(tree, sec::encode(tree.treeRootDetailLevel, 0, 0), 0);

    testSet(tree, sec::encode(tree.treeRootDetailLevel, -1, -1), -1, true);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 1, 1), -1, true);

    CHECK_MESSAGE(1 == countRootNodes(tree), "incorrect leaf value sum");
}

TEST_CASE("QuadTreeTest.TinyGridOffsetTreeTest") {
    MediumTestTree p;
    auto           treeP = makeTree(p, BlockPos2D(0, 0), BLOCK_DETAIL_LEVEL);
    IntTree&       tree  = *treeP;
    // minimum size tree should be 3 root nodes wide
    CHECK_MESSAGE(3 == tree.ringListWidth(), "incorrect tree node width");
    CHECK_MESSAGE(p.getWidthInBlocks() == tree.diameterInBlocks(), "incorrect tree width");

    // 2x2 valid positions (overlap the tree's width)
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 0, 0), 0);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, -1, 0), 0);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 0, -1), 0);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, -1, -1), 0);

    // invalid positions
    testSet(tree, sec::encode(tree.treeRootDetailLevel, -1, 1), -1, true);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 0, 1), -1, true);

    testSet(tree, sec::encode(tree.treeRootDetailLevel, 1, 0), -1, true);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 1, 1), -1, true);
    testSet(tree, sec::encode(tree.treeRootDetailLevel, 1, -1), -1, true);

    CHECK_MESSAGE(4 == countRootNodes(tree), "incorrect leaf value sum");
}

TEST_CASE("QuadTreeTest.TreeDetailLevelLimitTest") {
    MediumTestTree p;
    auto           treeP = makeTree(p, BlockPos2D(0, 0), 8);
    IntTree&       tree  = *treeP;
    CHECK_MESSAGE(
        10 == tree.treeRootDetailLevel,
        "Test detail level's need to be adjusted. This isn't necessarily a failed test."
    );

    // valid detail levels
    testSet(tree, sec::encode(10, 0, 0), 1);
    testSet(tree, sec::encode(9, 0, 0), 2);
    testSet(tree, sec::encode(8, 0, 0), 3);

    // detail level too low
    testSet(tree, sec::encode(7, 0, 0), -1, true);
    testSet(tree, sec::encode(6, 0, 0), -1, true);

    // detail level too high
    testSet(tree, sec::encode(11, 0, 0), -1, true);
    testSet(tree, sec::encode(12, 0, 0), -1, true);
}

namespace {
void recursivelyCreateNodeChildren(IntNode* node, std::int8_t minDetailLevel, int& minimumDetailLevelReached) {
    bool childNodesCreated  = false;
    bool childNodesIterated = false;

    // fill in the null children
    auto directChildIterator = node->getChildPosIterator();
    while (directChildIterator.hasNext()) {
        node->setValue(directChildIterator.nextLong(), 0);
        childNodesCreated = true;
    }

    // attempt to recurse down these new children
    directChildIterator = node->getChildPosIterator();
    while (directChildIterator.hasNext()) {
        Pos      sectionPos = directChildIterator.nextLong();
        IntNode* childNode  = node->getNode(sectionPos);

        REQUIRE(childNode != nullptr);
        CHECK_MESSAGE(
            sec::detail(childNode->sectionPos) >= minDetailLevel,
            "Child node recurred too low. Min detail level: ",
            (int)minDetailLevel
        );
        recursivelyCreateNodeChildren(childNode, minDetailLevel, minimumDetailLevelReached);

        childNodesIterated = true;
    }

    // keep track of how far down the tree we have gone
    if (sec::detail(node->sectionPos) < minimumDetailLevelReached) {
        minimumDetailLevelReached = sec::detail(node->sectionPos);
    }

    // assertions
    if (childNodesCreated) {
        CHECK_MESSAGE(
            sec::detail(node->sectionPos) >= minDetailLevel,
            "node children created below minimum detail level"
        );
    }
    if (childNodesIterated) {
        CHECK_MESSAGE(
            sec::detail(node->sectionPos) - 1 >= minDetailLevel,
            "node children iterated below minimum detail level"
        );
    }
}
} // namespace

TEST_CASE("QuadTreeTest.QuadNodeDetailLimitTest") {
    MediumTestTree p;
    auto           treeP = makeTree(p, BlockPos2D(0, 0), 6);
    IntTree&       tree  = *treeP;
    CHECK_MESSAGE(
        10 == tree.treeRootDetailLevel,
        "Test detail level's need to be adjusted. This isn't necessarily a failed test."
    );

    // create the root node
    testSet(tree, sec::encode(10, 0, 0), 1);

    int minimumDetailLevelReached = tree.treeRootDetailLevel;

    // recurse down the tree
    auto rootNodePosIterator = tree.rootNodePosIterator();
    while (rootNodePosIterator.hasNext()) {
        Pos      sectionPos = rootNodePosIterator.nextLong();
        IntNode* rootNode   = tree.getNode(sectionPos);
        if (rootNode != nullptr) {
            for (int i = 0; i < 4; i++) {
                // manually populate this direct child
                Pos rootChildPos = sec::child(rootNode->sectionPos, i);
                rootNode->setValue(rootChildPos, 0);
                // get the newly created child
                IntNode* childNode = rootNode->getChildByIndex(i);
                REQUIRE(childNode != nullptr);

                // walk through the tree for the real test
                recursivelyCreateNodeChildren(childNode, tree.treeLeafDetailLevel, minimumDetailLevelReached);
            }
        }
    }

    // confirm that the tree can and did iterate all the way down to the minimum detail level
    CHECK_MESSAGE(tree.treeLeafDetailLevel == minimumDetailLevelReached, "Incorrect minimum detail level reached.");
}

TEST_CASE("QuadTreeTest.quadNodeChildPositionIndexTest") {
    IntNode rootNode(sec::encode(10, 0, 0), 0);
    auto    directChildPosIterator = rootNode.getChildPosIterator();
    while (directChildPosIterator.hasNext()) {
        Pos sectionPos = directChildPosIterator.nextLong();
        CHECK_MESSAGE(
            sectionPos != rootNode.sectionPos,
            "Root node pos shouldn't be included in direct child pos iteration"
        );
        rootNode.setValue(sectionPos, 1);
    }
    CHECK_MESSAGE(4 == rootNode.getNonNullChildCount(), "node not filled");

    for (int i = 0; i < 4; i++) {
        Pos      childPos  = sec::child(rootNode.sectionPos, i);
        IntNode* childNode = rootNode.getChildByIndex(i);
        REQUIRE(childNode != nullptr);
        CHECK_MESSAGE(childPos == childNode->sectionPos, "child position not the same as sec::child()");
    }
}

TEST_CASE("QuadTreeTest.quadNodeChildPositionOutOfBoundsTest") {
    IntTree tree(64, 1, BlockPos2D(-2, 0), 0);

    // center root node
    Pos centerNodePos = sec::encode(1, 0, 0);

    // create node
    tree.setValue(centerNodePos, 0);
    IntNode* centerRootNode = tree.getNode(centerNodePos);
    REQUIRE(centerRootNode != nullptr);

    // child pos in bounds of the tree
    auto childPosIterator = centerRootNode->getChildPosIterator();
    while (childPosIterator.hasNext()) {
        Pos childPos = childPosIterator.nextLong();
        centerRootNode->setValue(childPos, 1);
    }
    CHECK_MESSAGE(4 == centerRootNode->getNonNullChildCount(), "center node not filled");

    // edge root node
    Pos offsetNodePos = sec::encode(1, -17, -16);

    // create node
    tree.setValue(offsetNodePos, 0);
    IntNode* offsetRootNode = tree.getNode(offsetNodePos);
    REQUIRE(offsetRootNode != nullptr);

    // child pos in bounds of the tree
    childPosIterator = offsetRootNode->getChildPosIterator();
    while (childPosIterator.hasNext()) {
        Pos childPos = childPosIterator.nextLong();
        offsetRootNode->setValue(childPos, 1);
    }
    CHECK_MESSAGE(4 == offsetRootNode->getNonNullChildCount(), "offset should contain all 4 children.");
}

// this is here for quickly testing the toString method, it should never fail
TEST_CASE("QuadTreeTest.toStringTest") {
    MediumTestTree p;
    auto           treeP      = makeTree(p, BlockPos2D(0, 0), 6);
    std::string    treeString = treeP->toString();
    CHECK(treeString != "");
}

//==================//
// extra: ring list //
//==================//

namespace {
using Ring = MovableGridRingList<std::shared_ptr<int>>;
std::shared_ptr<int> mk(int v) { return std::make_shared<int>(v); }
} // namespace

TEST_CASE("MovableGridRingList get/set/swap/remove and range") {
    Ring ring(2, 10, -3); // width 5, covers x 8..12, y -5..-1
    CHECK(ring.getWidth() == 5);
    CHECK(ring.getHalfWidth() == 2);
    CHECK(ring.getCenter() == GridPos2D(10, -3));
    CHECK(ring.getMinPosInRange() == GridPos2D(8, -5));
    CHECK(ring.getMaxPosInRange() == GridPos2D(12, -1));
    CHECK(ring.size() == 25);

    CHECK(ring.inRange(8, -5));
    CHECK(ring.inRange(12, -1));
    CHECK_FALSE(ring.inRange(7, -5));
    CHECK_FALSE(ring.inRange(13, -1));
    CHECK_FALSE(ring.inRange(8, 0));

    CHECK(ring.set(8, -5, mk(1)));
    CHECK(ring.set(GridPos2D(12, -1), mk(2)));
    CHECK_FALSE(ring.set(13, -1, mk(3)));
    CHECK(*ring.get(8, -5) == 1);
    CHECK(*ring.get(GridPos2D(12, -1)) == 2);
    CHECK(ring.get(13, -1) == nullptr);
    CHECK(ring.get(9, -5) == nullptr);

    auto old = ring.swap(8, -5, mk(10));
    REQUIRE(old);
    CHECK(*old == 1);
    CHECK(*ring.get(8, -5) == 10);
    auto rejected = ring.swap(100, 100, mk(99));
    CHECK(*rejected == 99); // outside the grid: the input is handed back
    auto removed = ring.remove(8, -5);
    CHECK(*removed == 10);
    CHECK(ring.get(8, -5) == nullptr);
    CHECK(ring.remove(100, 100) == nullptr);
}

TEST_CASE("MovableGridRingList moveTo keeps overlap and fires removal callbacks") {
    Ring ring(1, 0, 0); // width 3, x -1..1, y -1..1
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            ring.set(x, y, mk(x * 10 + y));
        }
    }

    CHECK_FALSE(ring.moveTo(0, 0)); // same centre -> false

    std::vector<int> removed;
    int              nullableCalls   = 0;
    int              nullableNonNull = 0;
    bool             moved           = ring.moveTo(
        1,
        0,
        [&](std::shared_ptr<int>& item) { removed.push_back(*item); },
        [&](const GridPos2D&, std::shared_ptr<int>& item) {
            nullableCalls++;
            if (item) nullableNonNull++;
        }
    );
    CHECK(moved);
    CHECK(ring.getCenter() == GridPos2D(1, 0));
    // x = -1 column (3 items) fell out of range
    CHECK(removed.size() == 3);
    std::sort(removed.begin(), removed.end());
    CHECK(removed == std::vector<int>{-11, -10, -9});
    CHECK(nullableCalls == 9);   // called for every cell
    CHECK(nullableNonNull == 3); // ... but only the removed ones carry an item

    // the kept overlap is still there at the same absolute positions
    CHECK(*ring.get(0, -1) == -1);
    CHECK(*ring.get(1, 1) == 11);
    // the newly covered column is empty, the old one out of range
    CHECK(ring.get(2, 0) == nullptr);
    CHECK(ring.get(-1, 0) == nullptr);
    CHECK(ring.inRange(2, 0));
    CHECK_FALSE(ring.inRange(-1, 0));

    // a move by >= width clears everything
    removed.clear();
    CHECK(ring.moveTo(50, 50, [&](std::shared_ptr<int>& item) { removed.push_back(*item); }));
    CHECK(removed.size() == 6);
    CHECK(ring.getCenter() == GridPos2D(50, 50));
    for (auto it = ring.begin(); it != ring.end(); ++it) {
        CHECK(*it == nullptr);
    }
}

TEST_CASE("MovableGridRingList negative coordinates wrap like floorMod") {
    Ring ring(1, -10, -10);
    CHECK(ring.set(-11, -11, mk(7)));
    CHECK(ring.set(-9, -9, mk(8)));
    CHECK(*ring.get(-11, -11) == 7);
    CHECK(*ring.get(-9, -9) == 8);
    ring.moveTo(-9, -9);
    CHECK(*ring.get(-9, -9) == 8);
    CHECK(ring.get(-11, -11) == nullptr);
    // (-11,-11) and (-8,-8) share a storage slot (mod 3) - moving must have cleared it
    CHECK(ring.get(-8, -8) == nullptr);
}

TEST_CASE("MovableGridRingList ordered iteration is in-to-out with stable ties") {
    Ring ring(1, 5, 5);
    for (int x = 4; x <= 6; x++)
        for (int y = 4; y <= 6; y++) ring.set(x, y, mk(x * 10 + y));
    ring.set(6, 6, nullptr); // forEachOrdered skips nulls, forEachPosOrdered does not

    std::vector<GridPos2D> posOrder;
    ring.forEachPosOrdered([&](std::shared_ptr<int>&, const GridPos2D& p) { posOrder.push_back(p); });
    const std::vector<GridPos2D> expected{
        {5, 5},
        {4, 5},
        {5, 4},
        {5, 6},
        {6, 5},
        {4, 4},
        {4, 6},
        {6, 4},
        {6, 6}
    };
    CHECK(posOrder == expected);

    std::vector<int> valueOrder;
    ring.forEachOrdered([&](std::shared_ptr<int>& v) { valueOrder.push_back(*v); });
    CHECK(valueOrder == std::vector<int>{55, 45, 54, 56, 65, 44, 46, 64});

    // forEachPos: x outer, y inner, all cells
    std::vector<GridPos2D> xMajor;
    ring.forEachPos([&](std::shared_ptr<int>&, const GridPos2D& p) { xMajor.push_back(p); });
    REQUIRE(xMajor.size() == 9);
    CHECK(xMajor.front() == GridPos2D(4, 4));
    CHECK(xMajor[1] == GridPos2D(4, 5));
    CHECK(xMajor.back() == GridPos2D(6, 6));
}

TEST_CASE("MovableGridRingList clear callback and value element type") {
    Ring ring(1, 0, 0);
    ring.set(0, 0, mk(1));
    ring.set(1, 1, mk(2));
    int sum = 0;
    ring.clear([&](std::shared_ptr<int>& item) { sum += *item; });
    CHECK(sum == 3);
    CHECK(ring.get(0, 0) == nullptr);

    MovableGridRingList<int> ints(1, 0, 0);
    CHECK(ints.set(0, 0, 5));
    CHECK(ints.get(0, 0) == 5);
    CHECK(ints.get(9, 9) == 0);
}

TEST_CASE("ArrayGridList and PosArrayGridList basics") {
    ArrayGridList<int> grid(3, [](int x, int y) { return x + y * 10; });
    CHECK(grid.size() == 9);
    CHECK(grid.get(2, 1) == 12);
    CHECK(grid.getFirst() == 0);
    CHECK(grid.getLast() == 22);
    CHECK(grid.get(3, 0) == 0); // outside: null (T{})
    CHECK(grid.set(1, 1, 99) == 11);
    CHECK(grid.get(1, 1) == 99);
    CHECK(grid.set(5, 5, 1) == 0);

    auto sub = ArrayGridList<int>::subGrid(grid, 1, 3);
    REQUIRE(sub.has_value());
    CHECK(sub->gridSize == 2);
    CHECK(sub->get(0, 0) == 99);
    CHECK(sub->get(1, 1) == 22);
    CHECK_FALSE(ArrayGridList<int>::subGrid(grid, 2, 1).has_value());
    CHECK_FALSE(ArrayGridList<int>::subGrid(grid, -1, 2).has_value());
    CHECK_FALSE(ArrayGridList<int>::subGrid(grid, 0, 4).has_value());

    PosArrayGridList<int> pos(3, 10, 20, [](int x, int y) { return x + y * 10; });
    // Java quirk: the filler sees 0-based coordinates
    CHECK(pos.get(10, 20) == 0);
    CHECK(pos.get(12, 21) == 12);
    CHECK(pos.get(0, 0) == 0); // outside the offset window
    CHECK(pos.inRange(12, 22));
    CHECK_FALSE(pos.inRange(13, 22));

    // move +1,+0: coordinates are preserved, the leaving column is dealloc'd, the entering column is null
    std::vector<int> dropped;
    CHECK(pos.move(1, 0, [&](int& v) { dropped.push_back(v); }));
    CHECK(pos.getOffsetX() == 11);
    CHECK(pos.getOffsetY() == 20);
    CHECK(dropped == std::vector<int>{0, 10, 20}); // old column x=10 (0-based filler values)
    CHECK(pos.get(11, 21) == 11);
    CHECK(pos.get(12, 21) == 12);
    CHECK(pos.get(13, 21) == 0);
    CHECK_FALSE(pos.move(0, 0, {}));
}

//=========================//
// extra: iteration order  //
//=========================//

namespace {
struct IterationDump {
    std::vector<Pos> roots, nodes, leaves;
    bool             operator==(const IterationDump&) const = default;
};

IterationDump dumpIteration(IntTree& tree) {
    IterationDump d;
    auto          r = tree.rootNodePosIterator();
    while (r.hasNext()) d.roots.push_back(r.nextLong());
    auto n = tree.nodeIterator();
    while (n.hasNext()) d.nodes.push_back(n.next()->sectionPos);
    auto l = tree.leafNodeIterator();
    while (l.hasNext()) d.leaves.push_back(l.next()->sectionPos);
    return d;
}
} // namespace

TEST_CASE("QuadTree iteration order is independent of insertion order") {
    // same content, inserted in two different (and one reversed) orders
    std::vector<Pos> positions;
    for (int rx = -1; rx <= 0; rx++) {
        for (int rz = -1; rz <= 0; rz++) {
            positions.push_back(sec::encode(10, rx, rz));
            for (int i = 0; i < 4; i++) {
                Pos c = sec::child(sec::encode(10, rx, rz), i);
                positions.push_back(c);
                positions.push_back(sec::child(c, (i + 1) % 4));
            }
        }
    }
    std::vector<Pos> reversed(positions.rbegin(), positions.rend());
    std::vector<Pos> rotated(positions);
    std::rotate(rotated.begin(), rotated.begin() + 7, rotated.end());

    auto build = [&](const std::vector<Pos>& order) {
        auto t = std::make_unique<IntTree>(8192, 16, BLOCK_POS2D_ZERO, 0);
        int  v = 0;
        for (Pos p : order) {
            // value derived from the position so it does not depend on insertion order
            v = static_cast<int>(sec::hashCode(p));
            REQUIRE(t->setValue(p, v));
        }
        return t;
    };

    auto a = build(positions);
    auto b = build(reversed);
    auto c = build(rotated);

    const IterationDump da = dumpIteration(*a);
    const IterationDump db = dumpIteration(*b);
    const IterationDump dc = dumpIteration(*c);
    CHECK(da == db);
    CHECK(da == dc);
    CHECK(da.roots.size() == 4);
    CHECK(!da.nodes.empty());
    CHECK(da.leaves.size() == 4 * 4);

    // node order: roots in ring order, within a root by detail level descending (BFS), children by index
    CHECK(sec::detail(da.nodes[0]) == a->treeRootDetailLevel);
    CHECK(da.nodes.size() == 4 * (3 + 1 + 4 + 4) /* chain 13..11, level 10, 4 children, 4 grandchildren */);
}
