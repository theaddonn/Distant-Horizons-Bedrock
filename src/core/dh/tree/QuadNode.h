#pragma once

// Port of util/objects/quadTree/QuadNode.java plus the iterators/ package
// (QuadTreeNodeIterator, QuadNodeChildIndexIterator, QuadNodeDirectChildIterator, QuadNodeDirectChildPosIterator).
//
// Child storage: children[i] is the child with section-pos child index i (sec::child(pos, i)):
//   0 = nwChild (rel 0,0)   1 = swChild (rel 1,0)   2 = neChild (rel 0,1)   3 = seChild (rel 1,1)
// (Java's names/Javadoc are inconsistent about N/S/E/W, but its getChildByIndex does exactly this mapping.)
//
// `value` is std::optional<T>: Java null == std::nullopt.
//
// Where Java throws IllegalArgumentException / IllegalStateException (getChildByIndex(!0..3), get/setValue with a
// position the node does not contain / of a bad detail level) this port assert()s in debug builds and returns
// nullptr / false in release builds without touching the tree.
//
// Iterators use Java's hasNext()/next() shape so the ported tests can stay 1:1. next() on an exhausted iterator
// asserts and returns nullptr / 0.

#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dh/pos/SectionPos.h"

namespace dh::tree {

template <class T>
class QuadNode;

/// QuadTree.INodeIteratorStoppingFunc: return true to stop walking down the tree at this node.
/// May be called with nullptr (root grid cells without a node), like Java.
template <class T>
using NodeStoppingFunc = std::function<bool(QuadNode<T>*)>;

template <class T>
class QuadNodeChildIndexIterator;
template <class T>
class QuadNodeDirectChildIterator;
template <class T>
class QuadNodeDirectChildPosIterator;
template <class T>
class QuadTreeNodeIterator;

template <class T>
class QuadNode {
public:
    using Value           = std::optional<T>;
    using RemovedConsumer = std::function<void(Value&)>;

    const sec::Pos sectionPos;
    /// the highest detail (smallest numeric level) this tree can provide, IE the leaf detail level of the owning tree
    const std::int8_t            parentTreeLeafDetailLevel;
    Value                        value;
    std::unique_ptr<QuadNode<T>> children[4];

    QuadNode(sec::Pos sectionPos_, std::int8_t parentTreeLeafDetailLevel_)
    : sectionPos(sectionPos_),
      parentTreeLeafDetailLevel(parentTreeLeafDetailLevel_) {}
    QuadNode(const QuadNode&)            = delete;
    QuadNode& operator=(const QuadNode&) = delete;

    //===========//
    // child get //
    //===========//

    /// @return the number of non-null direct child nodes
    int getDirectChildCount() const {
        int count = 0;
        for (const auto& c : children) {
            if (c) count++;
        }
        return count;
    }
    /// @return the number of non-null child nodes (identical to getDirectChildCount)
    int getTotalChildCount() const { return getDirectChildCount(); }
    /// @return the number of children that have non-null values (recursive)
    int getNonNullChildCount() const {
        int count = 0;
        for (const auto& c : children) {
            if (c && (c->value.has_value() || c->getNonNullChildCount() != 0)) {
                count++;
            }
        }
        return count;
    }

    /// Java throws IllegalArgumentException unless 0 <= child0to3 <= 3; here: assert + nullptr.
    QuadNode<T>*       getChildByIndex(int child0to3) { return childAt(child0to3); }
    const QuadNode<T>* getChildByIndex(int child0to3) const { return childAt(child0to3); }

    QuadNode<T>* nwChild() { return children[0].get(); }
    QuadNode<T>* swChild() { return children[1].get(); }
    QuadNode<T>* neChild() { return children[2].get(); }
    QuadNode<T>* seChild() { return children[3].get(); }

    /// @param pos must be contained by this node, not above it, and not below the tree's leaf level
    /// @return the node at the position, nullptr if it does not exist (or, in release, if pos is invalid)
    QuadNode<T>* getNode(sec::Pos pos) { return getOrSetNode(pos, false); }

    /// Creates the nodes down to `pos` if needed and replaces the value.
    /// @param previous if non-null receives the value before it was replaced
    /// @return false (nothing changed) if pos is invalid for this node (Java: IllegalArgumentException)
    bool setValue(sec::Pos pos, Value newValue, Value* previous = nullptr) {
        QuadNode<T>* node = getOrSetNode(pos, true);
        if (node == nullptr) {
            return false;
        }
        if (previous != nullptr) {
            *previous = std::move(node->value);
        }
        node->value = std::move(newValue);
        return true;
    }

    /// True if getNode/setValue accept this position (the conditions under which Java throws, negated).
    bool isValidPos(sec::Pos pos) const {
        if (!sec::contains(sectionPos, pos)) return false;
        if (sec::detail(pos) > sec::detail(sectionPos)) return false;
        if (sec::detail(pos) == sec::detail(sectionPos) && pos != sectionPos) return false;
        if (sec::detail(pos) < parentTreeLeafDetailLevel) return false;
        return true;
    }

    //===========//
    // iterators //
    //===========//

    /// Breadth first by detail level (all nodes, root first).
    QuadTreeNodeIterator<T> getNodeIterator() { return QuadTreeNodeIterator<T>(this, false, {}); }
    QuadTreeNodeIterator<T> getNodeIterator(const NodeStoppingFunc<T>& stop) {
        return QuadTreeNodeIterator<T>(this, false, stop);
    }
    /// Only nodes without children. Note a root without children is NOT returned.
    QuadTreeNodeIterator<T> getLeafNodeIterator() { return QuadTreeNodeIterator<T>(this, true, {}); }

    /// positions can point to null children
    QuadNodeDirectChildPosIterator<T> getChildPosIterator() { return QuadNodeDirectChildPosIterator<T>(this); }
    QuadNodeDirectChildIterator<T>    getChildNodeIterator() { return QuadNodeDirectChildIterator<T>(this); }

    //==========//
    // deletion //
    //==========//

    void deleteAllChildren() { deleteAllChildren(RemovedConsumer{}); }
    /// @param removedItemConsumer only fired for existing nodes, however the value passed in may be nullopt.
    ///        Order: all deeper levels first (child 0..3 recursion), then this node's children as 0, 2, 3, 1.
    void deleteAllChildren(const RemovedConsumer& removedItemConsumer) {
        for (int i = 0; i < 4; i++) {
            if (children[i]) {
                children[i]->deleteAllChildren(removedItemConsumer);
            }
        }
        for (int i : {0, 2, 3, 1}) { // nw, ne, se, sw
            if (children[i] && removedItemConsumer) {
                removedItemConsumer(children[i]->value);
            }
            children[i].reset();
        }
    }

    //==============//
    // base methods //
    //==============//

    /// "pos: 6*1,-3, children #: 2, value: 5"
    std::string toString() const {
        std::string v = "null";
        if (value.has_value()) {
            if constexpr (std::is_arithmetic_v<T>) {
                v = std::to_string(*value);
            } else {
                v = "<set>";
            }
        }
        return "pos: " + sec::toString(sectionPos) + ", children #: " + std::to_string(getTotalChildCount())
             + ", value: " + v;
    }

private:
    QuadNode<T>* childAt(int i) const {
        assert(i >= 0 && i <= 3);
        if (i < 0 || i > 3) return nullptr;
        return children[i].get();
    }

    QuadNode<T>* getOrSetNode(sec::Pos inputSectionPos, bool create) {
        if (!isValidPos(inputSectionPos)) {
            assert(false && "QuadNode: position outside of / incompatible with this node");
            return nullptr;
        }

        QuadNode<T>* node = this;
        while (sec::detail(inputSectionPos) != sec::detail(node->sectionPos)) {
            // this node is a parent of the requested position, descend into the child containing it
            int next = -1;
            for (int i = 0; i < 4; i++) {
                if (sec::contains(sec::child(node->sectionPos, i), inputSectionPos)) {
                    next = i;
                    break;
                }
            }
            if (next < 0) {
                assert(false && "QuadNode: input position not contained by any node children");
                return nullptr;
            }
            if (!node->children[next]) {
                if (!create) {
                    return nullptr; // end of the node chain
                }
                node->children[next] =
                    std::make_unique<QuadNode<T>>(sec::child(node->sectionPos, next), node->parentTreeLeafDetailLevel);
            }
            node = node->children[next].get();
        }
        return node;
    }
};

//==================//
// iterator classes //
//==================//

/// Child indices (ascending) of existing children - or of all 4 slots when returnNullChildPos - and none at all
/// when the parent already is at the tree's leaf detail level.
template <class T>
class QuadNodeChildIndexIterator {
public:
    QuadNodeChildIndexIterator(const QuadNode<T>* parentNode, bool returnNullChildPos) {
        if (sec::detail(parentNode->sectionPos) > parentNode->parentTreeLeafDetailLevel) {
            for (int i = 0; i < 4; i++) {
                if (returnNullChildPos || parentNode->getChildByIndex(i) != nullptr) {
                    indices_.push_back(i);
                }
            }
        }
    }
    bool hasNext() const { return pos_ < indices_.size(); }
    int  next() {
        assert(hasNext());
        return hasNext() ? indices_[pos_++] : -1;
    }

private:
    std::vector<int> indices_;
    std::size_t      pos_ = 0;
};

template <class T>
class QuadNodeDirectChildIterator {
public:
    explicit QuadNodeDirectChildIterator(QuadNode<T>* parentNode) : parent_(parentNode), idx_(parentNode, false) {}
    bool         hasNext() const { return idx_.hasNext(); }
    QuadNode<T>* next() {
        if (!hasNext()) {
            assert(false);
            return nullptr;
        }
        return parent_->getChildByIndex(idx_.next());
    }

private:
    QuadNode<T>*                  parent_;
    QuadNodeChildIndexIterator<T> idx_;
};

/// Positions of the direct children, including positions whose child node does not exist.
template <class T>
class QuadNodeDirectChildPosIterator {
public:
    explicit QuadNodeDirectChildPosIterator(const QuadNode<T>* parentNode)
    : parent_(parentNode),
      idx_(parentNode, true) {}
    bool     hasNext() const { return idx_.hasNext(); }
    sec::Pos nextLong() {
        if (!hasNext()) {
            assert(false);
            return 0;
        }
        return sec::child(parent_->sectionPos, idx_.next());
    }

private:
    const QuadNode<T>*            parent_;
    QuadNodeChildIndexIterator<T> idx_;
};

/// All-nodes mode: breadth first, one detail level after the other, children in index order, root first; the
/// next level is only built when the current one has been handed out. Leaf mode: the queue is built up front
/// by a BFS and holds every node that has no children (the start node itself is never returned).
template <class T>
class QuadTreeNodeIterator {
public:
    QuadTreeNodeIterator(QuadNode<T>* rootNode, bool onlyReturnLeafValues, const NodeStoppingFunc<T>& stopIteratingFunc)
    : leafDetailLevel_(rootNode->parentTreeLeafDetailLevel),
      iteratorDetailLevel_(sec::detail(rootNode->sectionPos)),
      onlyReturnLeafValues_(onlyReturnLeafValues),
      stop_(stopIteratingFunc) {
        if (!onlyReturnLeafValues_) {
            validNodesForDetailLevel_.push_back(rootNode);
            iteratorNodeQueue_.push_back(rootNode);
        } else {
            std::deque<QuadNode<T>*> parentNodeQueue;
            parentNodeQueue.push_back(rootNode);
            while (!parentNodeQueue.empty()) {
                QuadNode<T>* parentNode = parentNodeQueue.front();
                parentNodeQueue.pop_front();

                if (stop_ && stop_(parentNode)) {
                    continue;
                }
                for (int i = 0; i < 4; i++) {
                    QuadNode<T>* childNode = parentNode->getChildByIndex(i);
                    if (childNode != nullptr) {
                        if (childNode->getTotalChildCount() == 0) {
                            iteratorNodeQueue_.push_back(childNode);
                        } else {
                            parentNodeQueue.push_back(childNode);
                        }
                    }
                }
            }
        }
    }

    bool hasNext() const { return !iteratorNodeQueue_.empty(); }

    QuadNode<T>* next() {
        if (iteratorDetailLevel_ < leafDetailLevel_ || iteratorNodeQueue_.empty()) {
            assert(false && "QuadTreeNodeIterator: no more elements");
            return nullptr;
        }

        QuadNode<T>* currentNode = iteratorNodeQueue_.front();
        iteratorNodeQueue_.pop_front();

        // for all-node mode the queue has to be refilled once per detail level
        if (iteratorNodeQueue_.empty() && !onlyReturnLeafValues_) {
            iteratorDetailLevel_ = static_cast<std::int8_t>(iteratorDetailLevel_ - 1);
            if (iteratorDetailLevel_ >= leafDetailLevel_) {
                std::deque<QuadNode<T>*> parentNodes = std::move(validNodesForDetailLevel_);
                validNodesForDetailLevel_.clear();

                for (QuadNode<T>* parentNode : parentNodes) {
                    if (stop_ && stop_(parentNode)) {
                        continue;
                    }
                    for (int i = 0; i < 4; i++) {
                        QuadNode<T>* childNode = parentNode->getChildByIndex(i);
                        if (childNode == nullptr) {
                            continue;
                        }
                        if (stop_ && stop_(childNode)) {
                            continue;
                        }
                        iteratorNodeQueue_.push_back(childNode);
                        validNodesForDetailLevel_.push_back(childNode);
                    }
                }
            }
        }
        return currentNode;
    }

private:
    std::int8_t              leafDetailLevel_;
    std::deque<QuadNode<T>*> validNodesForDetailLevel_;
    std::deque<QuadNode<T>*> iteratorNodeQueue_;
    std::int8_t              iteratorDetailLevel_;
    bool                     onlyReturnLeafValues_;
    NodeStoppingFunc<T>      stop_;
};

} // namespace dh::tree
