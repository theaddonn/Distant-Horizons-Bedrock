#pragma once

// Port of render/QuadTree/QuadTreeTickNodeHolder.java: everything LodQuadTree::tryTick collects while walking
// the tree, so different logic can run at different times for each node.
//
// Java used HashSets (iteration order = hash order). Here every list keeps INSERTION order (the tree walk
// order, which is deterministic) and a separate membership set only answers contains(); nothing iterates a
// hash container. The invariant Java relied on holds: a node is in at most one of enable / disable /
// enableDeleteChildren, and once a node is "present" it cannot move to another list except through
// removeEnableAndDisableNode (enable -> disable).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "dh/pos/BlockPos.h"
#include "dh/pos/SectionPos.h"
#include "dh/tree/LodRenderSection.h"
#include "dh/tree/QuadNode.h"

namespace dh::tree {

class TickNodeHolder {
public:
    using Section = std::shared_ptr<LodRenderSection>;
    using Node    = QuadNode<Section>;

    void clear() {
        sectionsToLoad_.clear();
        loadSet_.clear();
        presentNodes_.clear();
        enableNodes_.clear();
        enableSet_.clear();
        disableNodes_.clear();
        disableSet_.clear();
        enableDeleteChildren_.clear();
        enableDeleteChildrenSet_.clear();
    }

    // loading (HashSet<LodRenderSection>: deduplicated)
    void addLoadSection(const Section& section) {
        if (loadSet_.insert(section.get()).second) {
            sectionsToLoad_.push_back(section);
        }
    }
    const std::vector<Section>& getLoadSections() const { return sectionsToLoad_; }

    // enable
    void addEnableNode(Node* node) {
        if (presentNodes_.insert(node).second) {
            enableNodes_.push_back(node);
            enableSet_.insert(node);
        }
    }
    /// The node shouldn't be rendered since it's being disabled by a parent.
    void removeEnableAndDisableNode(Node* node) {
        if (node == nullptr) {
            return; // Java added null to its HashSets and skipped it later
        }
        enableSet_.erase(node);
        if (enableDeleteChildrenSet_.erase(node) != 0) {
            std::erase(enableDeleteChildren_, node);
        }
        presentNodes_.insert(node); // should already be present, but re-added just in case
        if (disableSet_.insert(node).second) {
            disableNodes_.push_back(node);
        }
    }
    /// Java getEnabledNodes().contains(node); false for nullptr.
    bool isEnableNode(const Node* node) const { return node != nullptr && enableSet_.contains(node); }
    /// Nodes to enable, in the order they were added (nodes moved to disable are left out).
    std::vector<Node*> getEnabledNodes() const {
        std::vector<Node*> out;
        out.reserve(enableNodes_.size());
        for (Node* n : enableNodes_) {
            if (enableSet_.contains(n)) out.push_back(n);
        }
        return out;
    }

    // disable
    void addDisableNode(Node* node) {
        if (presentNodes_.insert(node).second) {
            disableSet_.insert(node);
            disableNodes_.push_back(node);
        }
    }
    const std::vector<Node*>& getDisableNodes() const { return disableNodes_; }

    // enable - delete children
    void addEnableDeleteChildrenNode(Node* node) {
        if (presentNodes_.insert(node).second) {
            enableDeleteChildrenSet_.insert(node);
            enableDeleteChildren_.push_back(node);
        }
    }
    const std::vector<Node*>& getEnableDeleteChildrenNodes() const { return enableDeleteChildren_; }

    /// Java getWorldGenNodesNearToFar: the enable-delete-children nodes' sections, closest first
    /// (stable sort by manhattan block distance, like List.sort). Returns sections (not nodes) because the
    /// nodes may be deleted before the asynchronous queuing task reads them.
    std::vector<Section> getWorldGenSectionsNearToFar(BlockPos2D centerPos) const {
        std::vector<Node*> sorted = enableDeleteChildren_;
        std::stable_sort(sorted.begin(), sorted.end(), [&](const Node* a, const Node* b) {
            return sec::manhattanBlockDistance(a->sectionPos, centerPos)
                 < sec::manhattanBlockDistance(b->sectionPos, centerPos);
        });
        std::vector<Section> out;
        out.reserve(sorted.size());
        for (Node* n : sorted) {
            if (n->value.has_value() && *n->value) out.push_back(*n->value);
        }
        return out;
    }

private:
    std::vector<Section>            sectionsToLoad_;
    std::unordered_set<const void*> loadSet_;
    std::unordered_set<const Node*> presentNodes_;
    std::vector<Node*>              enableNodes_;
    std::unordered_set<const Node*> enableSet_;
    std::vector<Node*>              disableNodes_;
    std::unordered_set<const Node*> disableSet_;
    std::vector<Node*>              enableDeleteChildren_;
    std::unordered_set<const Node*> enableDeleteChildrenSet_;
};

} // namespace dh::tree
