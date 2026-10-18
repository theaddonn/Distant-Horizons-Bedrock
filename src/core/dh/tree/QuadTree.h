#pragma once

// Port of util/objects/quadTree/QuadTree.java.
//
// A QuadTree<T> is a MovableGridRingList of root QuadNodes (always a 3x3 ring grid for power-of-two diameters)
// each of which grows a quad tree of std::optional<T> values down to treeLeafDetailLevel.
//
// Java null <-> std::nullopt. Where Java throws IndexOutOfBoundsException for an out of bounds position
// (getValue/getNode/setValue) this port returns false from the checked overloads (nothing is modified) and
// nullptr / nullopt from the unchecked ones; it never asserts for out-of-bounds input because callers use
// isSectionPosInBounds() to probe. Internal invariants Java guards with LodUtil.assertNotReach are assert()ed.
//
// Iteration order (deterministic, equal to Java):
//   * root grid: in-to-out ring order of the 3x3 root grid (distance from the grid centre, ties x-major)
//   * nodeIterator(): per root, breadth first by detail level, children in index order 0..3
//   * leafNodeIterator(): per root, breadth first discovery order of childless nodes (see QuadTreeNodeIterator)
//   * nodeCount()/leafNodeCount() visit the roots in ring-STORAGE order (only matters for the callbacks' order
//     in setCenterBlockPos' mutate pass, which also uses storage order, like Java's ArrayList.forEach).

#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "dh/pos/BlockPos.h"
#include "dh/pos/SectionPos.h"
#include "dh/tree/GridRing.h"
#include "dh/tree/QuadNode.h"
#include "dh/util/Bits.h"
#include "dh/util/MathUtil.h"

namespace dh::tree {

namespace detail {
/// QuadTree.DoSquaresOverlap: the Java code compares as float; so do we.
bool doSquaresOverlap(sec::Pos square1Min, std::int32_t square1Width, sec::Pos square2Min, std::int32_t square2Width);
} // namespace detail

template <class T>
class QuadTree {
public:
    using Node                      = QuadNode<T>;
    using Value                     = std::optional<T>;
    using RemovedConsumer           = std::function<void(Value&)>;
    using INodeIteratorStoppingFunc = NodeStoppingFunc<T>;

    /// IE: the detail level used by the root nodes (largest numerical detail level).
    const std::int8_t treeRootDetailLevel;
    /// IE: the detail level used by the leaf nodes (smallest numerical detail level).
    const std::int8_t treeLeafDetailLevel;

    /// @param diameterInBlocks distance between two opposing sides
    /// @param blockDistanceForNodeClearing how far (manhattan) the centre must move before out-of-bound nodes are
    /// re-checked
    QuadTree(
        std::int32_t diameterInBlocks,
        std::int32_t blockDistanceForNodeClearing,
        BlockPos2D   centerBlockPos,
        std::int8_t  treeLeafDetailLevel_
    )
    : treeRootDetailLevel(computeRootDetailLevel(treeLeafDetailLevel_, diameterInBlocks)),
      treeLeafDetailLevel(treeLeafDetailLevel_),
      diameterInBlocks_(diameterInBlocks),
      centerBlockPos_(centerBlockPos),
      blockDistanceForNodeClearing_(blockDistanceForNodeClearing),
      topRingList_(
          // always add 1 so nodes always have a parent, even if the tree's centre is offset from the root grid
          wadd(jdiv(jfloordiv(diameterInBlocks, 2), powerOfTwo(treeRootDetailLevel)), 1),
          divideByPowerOfTwo(centerBlockPos.x, treeRootDetailLevel),
          divideByPowerOfTwo(centerBlockPos.z, treeRootDetailLevel)
      ) {}
    QuadTree(const QuadTree&)            = delete;
    QuadTree& operator=(const QuadTree&) = delete;

    //==================//
    // property getters //
    //==================//

    std::int8_t treeMinDetailLevel() const { return treeLeafDetailLevel; }
    std::int8_t treeMaxDetailLevel() const { return treeRootDetailLevel; }

    /// @return the number of nodes holding a value
    int nodeCount() {
        int count = 0;
        topRingList_.forEach([&](std::shared_ptr<Node>& root) {
            if (!root) return;
            auto it = root->getNodeIterator();
            while (it.hasNext()) {
                if (it.next()->value.has_value()) count++;
            }
        });
        return count;
    }
    int count() { return nodeCount(); }

    /// @return the number of (in bounds) leaf nodes in the tree
    int leafNodeCount() {
        int count = 0;
        topRingList_.forEach([&](std::shared_ptr<Node>& root) {
            if (!root) return;
            auto it = root->getLeafNodeIterator();
            while (it.hasNext()) {
                Node* node = it.next();
                if (node != nullptr && isSectionPosInBounds(node->sectionPos)) count++;
            }
        });
        return count;
    }

    /// A tree always has a 3x3 root node ring (Java hard codes this; kept as is).
    int          ringListWidth() const { return 3; }
    int          ringListHalfWidth() const { return 1; }
    std::int32_t diameterInBlocks() const { return diameterInBlocks_; }

    bool isEmpty() { return nodeCount() == 0; }

    //==========================//
    // node getters and setters //
    //==========================//

    /// @return the node at pos; nullptr if pos is out of bounds or the node does not exist
    Node* tryGetNode(sec::Pos pos) {
        if (!isSectionPosInBounds(pos)) return nullptr;
        Node* root = rootNodeFor(pos, false);
        return root != nullptr ? root->getNode(pos) : nullptr;
    }
    /// Java getNode() throws IndexOutOfBoundsException when out of bounds: use isSectionPosInBounds or the
    /// overload below to tell "out of bounds" from "missing".
    Node* getNode(sec::Pos pos) { return tryGetNode(pos); }
    /// @return false if pos is out of bounds (Java: IndexOutOfBoundsException); `out` is nullptr if missing.
    bool getNode(sec::Pos pos, Node*& out) {
        out = nullptr;
        if (!isSectionPosInBounds(pos)) return false;
        out = tryGetNode(pos);
        return true;
    }

    /// @return the value at pos; nullopt if missing or out of bounds
    Value tryGetValue(sec::Pos pos) {
        Node* node = tryGetNode(pos);
        return node != nullptr ? node->value : Value{};
    }
    Value getValue(sec::Pos pos) { return tryGetValue(pos); }
    /// @return false if pos is out of bounds (Java: IndexOutOfBoundsException); `out` is nullopt if missing.
    bool getValue(sec::Pos pos, Value& out) {
        out = std::nullopt;
        if (!isSectionPosInBounds(pos)) return false;
        out = tryGetValue(pos);
        return true;
    }

    /// Creates the nodes down to pos if needed (even when newValue is nullopt, like Java).
    /// @param previous receives the value that was at pos (nullopt if none)
    /// @return false and nothing changes if pos is out of bounds (Java: IndexOutOfBoundsException)
    bool setValue(sec::Pos pos, Value newValue, Value* previous = nullptr) {
        if (!isSectionPosInBounds(pos)) return false;
        Node* root = rootNodeFor(pos, true);
        if (root == nullptr) return false; // Java: LodUtil.assertNotReach
        return root->setValue(pos, std::move(newValue), previous);
    }
    /// setValue(pos, nullopt): the node itself stays in the tree.
    bool deleteValue(sec::Pos pos, Value* previous = nullptr) { return setValue(pos, Value{}, previous); }

    bool isSectionPosInBounds(sec::Pos testPos) const {
        // detail level within the tree's limits?
        const std::int8_t d = sec::detail(testPos);
        if (!(treeLeafDetailLevel <= d && d <= treeRootDetailLevel)) {
            return false;
        }

        // X/Z overlap with the tree's square?
        const std::int32_t half            = jdiv(wneg(diameterInBlocks_), 2); // Java: -diameterInBlocks / 2
        const BlockPos2D   treeBlockCorner = centerBlockPos_.add(BlockPos2D(half, half));
        const sec::Pos     treeCornerPos   = sec::encode(0, treeBlockCorner.x, treeBlockCorner.z);

        const sec::Pos     inputSectionCorner = sec::convertToDetail(testPos, 0);
        const sec::Pos     inputCornerPos     = sec::encode(0, sec::x(inputSectionCorner), sec::z(inputSectionCorner));
        const std::int32_t inputBlockWidth    = powerOfTwo(d);

        return detail::doSquaresOverlap(treeCornerPos, diameterInBlocks_, inputCornerPos, inputBlockWidth);
    }

    int getNonNullChildCountAtPos(sec::Pos pos) { return getChildCountAtPos(pos, false); }
    int getChildCountAtPos(sec::Pos pos, bool includeNullValues) {
        int childCount = 0;
        for (int i = 0; i < 4; i++) {
            const sec::Pos childPos = sec::child(pos, i);
            if (isSectionPosInBounds(childPos)) {
                const Value value = getValue(childPos);
                if (includeNullValues || value.has_value()) childCount++;
            }
        }
        return childCount;
    }

    //===========//
    // iterators //
    //===========//

    /// Positions of the root grid cells (in-to-out ring order) that are in bounds; can include null nodes.
    class RootPosIterator {
    public:
        RootPosIterator(QuadTree& tree, bool includeNullNodes, const INodeIteratorStoppingFunc& stop) {
            tree.topRingList_.forEachPosOrdered([&](std::shared_ptr<Node>& node, const GridPos2D& pos2D) {
                if (stop && stop(node.get())) {
                    return;
                }
                if (node || includeNullNodes) {
                    const sec::Pos rootPos = sec::encode(tree.treeRootDetailLevel, pos2D.x, pos2D.y);
                    if (tree.isSectionPosInBounds(rootPos)) {
                        queue_.push_back(rootPos);
                    }
                }
            });
        }
        bool     hasNext() const { return !queue_.empty(); }
        sec::Pos nextLong() {
            if (queue_.empty()) {
                assert(false && "NoSuchElementException");
                return 0;
            }
            const sec::Pos p = queue_.front();
            queue_.pop_front();
            return p;
        }

    private:
        std::deque<sec::Pos> queue_;
    };

    /// Walks every root's tree; roots are visited in RootPosIterator order.
    class NodeIterator {
    public:
        NodeIterator(QuadTree& tree, bool onlyReturnLeaves, const INodeIteratorStoppingFunc& stop)
        : tree_(&tree),
          roots_(tree, false, stop),
          onlyReturnLeaves_(onlyReturnLeaves),
          stop_(stop) {}

        bool hasNext() {
            if (!roots_.hasNext() && current_.has_value() && !current_->hasNext()) {
                return false;
            }
            if (!current_.has_value() || !current_->hasNext()) {
                current_ = nextChildNodeIterator();
            }
            return current_.has_value() && current_->hasNext();
        }
        Node* next() {
            if (!current_.has_value() || !current_->hasNext()) {
                current_ = nextChildNodeIterator();
            }
            if (!current_.has_value() || !current_->hasNext()) {
                assert(false && "NoSuchElementException");
                return nullptr;
            }
            lastNode_ = current_->next();
            return lastNode_;
        }
        /// Iterator.remove(): clears the last returned node's value and deletes all of its children.
        /// The iterator stays valid: queued descendants of the removed node are purged first.
        void remove() {
            if (lastNode_ == nullptr) {
                assert(false && "No last node found.");
                return;
            }
            Node* node = tree_->setNodeValueCreating(lastNode_->sectionPos);
            if (node != nullptr) {
                if (current_.has_value()) {
                    current_->purgeDescendantsOf(lastNode_->sectionPos);
                }
                node->deleteAllChildren();
            }
        }

    private:
        std::optional<QuadTreeNodeIterator<T>> nextChildNodeIterator() {
            std::optional<QuadTreeNodeIterator<T>> nodeIterator;
            while ((!nodeIterator.has_value() || !nodeIterator->hasNext()) && roots_.hasNext()) {
                const sec::Pos sectionPos = roots_.nextLong();
                Node*          rootNode   = tree_->tryGetNode(sectionPos);
                if (rootNode != nullptr) {
                    nodeIterator =
                        onlyReturnLeaves_ ? rootNode->getLeafNodeIterator() : rootNode->getNodeIterator(stop_);
                }
            }
            return nodeIterator;
        }

        QuadTree*                              tree_;
        RootPosIterator                        roots_;
        std::optional<QuadTreeNodeIterator<T>> current_;
        Node*                                  lastNode_ = nullptr;
        bool                                   onlyReturnLeaves_;
        INodeIteratorStoppingFunc              stop_;
    };

    RootPosIterator rootNodePosIterator() { return RootPosIterator(*this, true, {}); }
    NodeIterator    nodeIteratorWithStoppingFilter(const INodeIteratorStoppingFunc& stop) {
        return NodeIterator(*this, false, stop);
    }
    NodeIterator nodeIterator() { return NodeIterator(*this, false, {}); }
    NodeIterator leafNodeIterator() { return NodeIterator(*this, true, {}); }

    //================//
    // get/set center //
    //================//

    void setCenterBlockPos(BlockPos2D newCenterPos) {
        setCenterBlockPos(newCenterPos, RemovedConsumer{}, RemovedConsumer{});
    }
    /// @param removedConsumer fired when a root node is completely removed from the underlying data structure
    ///        (for every value of the removed tree, children first, then the root's value; values may be nullopt)
    /// @param mutateOutOfBoundConsumer fired (bottom up) for non-null values of nodes that are out of bounds
    ///        but kept in the tree
    void setCenterBlockPos(
        BlockPos2D             newCenterPos,
        const RemovedConsumer& removedConsumer,
        const RemovedConsumer& mutateOutOfBoundConsumer
    ) {
        // did we move significantly?
        const std::int32_t newCenterPosX = divideByPowerOfTwo(newCenterPos.x, treeRootDetailLevel);
        const std::int32_t newCenterPosZ = divideByPowerOfTwo(newCenterPos.z, treeRootDetailLevel);
        const bool         ringListMoved =
            topRingList_.getCenter().x != newCenterPosX || topRingList_.getCenter().y != newCenterPosZ;

        // did we move a little bit?
        const bool recalculateOutOfBoundNodes =
            centerBlockPos_.manhattanDist(newCenterPos) >= blockDistanceForNodeClearing_;

        if (!ringListMoved && !recalculateOutOfBoundNodes) {
            // NOTE: like Java the stored centre is NOT updated in this case
            return;
        }

        centerBlockPos_ = newCenterPos;

        // remove out of bound root nodes
        topRingList_.moveTo(newCenterPosX, newCenterPosZ, [&](std::shared_ptr<Node>& quadNode) {
            if (quadNode) {
                quadNode->deleteAllChildren(removedConsumer);
                if (removedConsumer) {
                    removedConsumer(quadNode->value);
                }
            }
        });

        // mutate out of bound child nodes (ring storage order, includes null cells)
        topRingList_.forEach([&](std::shared_ptr<Node>& rootNode) {
            mutateOutOfBoundChildNodes(rootNode.get(), mutateOutOfBoundConsumer);
        });
    }

    const BlockPos2D& getCenterBlockPos() const { return centerBlockPos_; }

    /// Removes every root node (and so every node). removedConsumer: see setCenterBlockPos.
    void clear(const RemovedConsumer& removedConsumer = {}) {
        topRingList_.clear([&](std::shared_ptr<Node>& quadNode) {
            if (quadNode) {
                quadNode->deleteAllChildren(removedConsumer);
                if (removedConsumer) removedConsumer(quadNode->value);
            }
        });
    }

    //==============//
    // base methods //
    //==============//

    std::string toString() {
        return "center block: " + centerBlockPos_.toString() + ", block width: " + std::to_string(diameterInBlocks_)
             + ", detail level range: [" + std::to_string(treeLeafDetailLevel) + "-"
             + std::to_string(treeRootDetailLevel) + "], leaf #: " + std::to_string(leafNodeCount());
    }

private:
    static std::int8_t computeRootDetailLevel(std::int8_t leaf, std::int32_t diameterInBlocks) {
        // must be > 0 (to prevent divide by 0 errors) and >= the leaf detail level
        const std::int32_t a = leaf > 1 ? leaf : 1;
        const std::int32_t b = math::log2(diameterInBlocks);
        return static_cast<std::int8_t>(a > b ? a : b);
    }

    /// root node covering `pos` (in bounds); created when `create`. nullptr if missing / not creatable.
    Node* rootNodeFor(sec::Pos pos, bool create) {
        const sec::Pos     rootPos      = sec::convertToDetail(pos, treeRootDetailLevel);
        const std::int32_t ringListPosX = sec::x(rootPos);
        const std::int32_t ringListPosZ = sec::z(rootPos);

        std::shared_ptr<Node>* slot = topRingList_.getPtr(ringListPosX, ringListPosZ);
        if (slot == nullptr) {
            assert(!create && "Failed to add root quadTree node: outside the root grid");
            return nullptr;
        }
        if (!*slot) {
            if (!create) return nullptr;
            *slot = std::make_shared<Node>(rootPos, treeLeafDetailLevel);
        }
        assert(sec::contains((*slot)->sectionPos, pos) && "failed to get a root node that contains the input position");
        return slot->get();
    }

    /// (iterator remove) getOrSetNode(pos, set=true, null, throw=false): creates the node, sets its value to null.
    Node* setNodeValueCreating(sec::Pos pos) {
        if (!isSectionPosInBounds(pos)) return nullptr;
        Node* root = rootNodeFor(pos, true);
        if (root == nullptr) return nullptr;
        Node* node = root->getNode(pos);
        if (!root->setValue(pos, Value{})) return nullptr;
        return node != nullptr ? node : root->getNode(pos);
    }

    /// We don't remove nodes or node data, since that causes the LOD tree to flash low-detail LODs when moving
    /// into previously-loaded LODs. Mutates bottom up.
    void mutateOutOfBoundChildNodes(Node* quadNode, const RemovedConsumer& mutateOutOfBoundConsumer) {
        if (quadNode == nullptr) return;
        for (int i = 0; i < 4; i++) {
            Node* childNode = quadNode->getChildByIndex(i);
            if (childNode == nullptr || !childNode->value.has_value()) {
                continue; // no need to go any deeper if this node is already empty
            }
            mutateOutOfBoundChildNodes(childNode, mutateOutOfBoundConsumer);
            if (!isSectionPosInBounds(childNode->sectionPos)) {
                if (mutateOutOfBoundConsumer) mutateOutOfBoundConsumer(childNode->value);
            }
        }
    }

    std::int32_t                               diameterInBlocks_;
    BlockPos2D                                 centerBlockPos_;
    std::int32_t                               blockDistanceForNodeClearing_;
    MovableGridRingList<std::shared_ptr<Node>> topRingList_;
};

} // namespace dh::tree
