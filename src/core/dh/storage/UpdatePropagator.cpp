#include "dh/storage/UpdatePropagator.h"

#include "dh/fulldata/Merge.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace dh {

namespace {

void logErr(const char* what, std::uint64_t pos, const Error& e) {
    log::logf(
        LogLevel::Error,
        "%s at section [%d,%d,%d]: %s",
        what,
        static_cast<int>(sec::detail(pos)),
        static_cast<int>(sec::x(pos)),
        static_cast<int>(sec::z(pos)),
        e.message.c_str()
    );
}

} // namespace

UpdatePropagator::UpdatePropagator(SourceProvider& provider, IExecutor& executor, int threadCount)
: provider_(provider),
  updater_(provider.updater()),
  executor_(executor),
  maxTasks_(
      static_cast<std::size_t>(NUMBER_OF_PARENT_UPDATE_TASKS_PER_THREAD)
      * static_cast<std::size_t>(std::max(threadCount, 1))
  ) {}

std::size_t UpdatePropagator::updatingCount() const {
    std::lock_guard lock(updatingMutex_);
    return updating_.size();
}

bool UpdatePropagator::tryBeginPos(std::uint64_t pos) {
    std::lock_guard lock(updatingMutex_);
    return updating_.insert(pos).second;
}

void UpdatePropagator::endPos(std::uint64_t pos) {
    std::lock_guard lock(updatingMutex_);
    updating_.erase(pos);
}

UpdatePropagator::TickStats UpdatePropagator::tick(BlockPos2D target) {
    TickStats s;
    s.parentTasks = runParentUpdates(target);
    s.childTasks  = runChildUpdates(target);
    return s;
}

//================//
// parent updates //
//================//

std::size_t UpdatePropagator::runParentUpdates(BlockPos2D target) {
    if (provider_.isClosed()) return 0;

    // Queue parent updates unless plenty are already pending.
    if (executor_.queueSize() > maxTasks_ || updatingCount() > maxTasks_) return 0;

    auto childList = provider_.store().positionsToPropagateUp(target.x, target.z, maxTasks_);
    if (!childList) {
        log::logf(LogLevel::Error, "positionsToPropagateUp failed: %s", childList.error().message.c_str());
        return 0;
    }

    // Combine updates by parent: parent order = first appearance in the nearest-first list,
    // children keep list order.
    std::vector<std::uint64_t>                          parentOrder;
    std::map<std::uint64_t, std::vector<std::uint64_t>> childrenByParent;
    for (std::uint64_t child : *childList) {
        const std::uint64_t parent = sec::parent(child);
        auto [it, inserted]        = childrenByParent.try_emplace(parent);
        if (inserted) parentOrder.push_back(parent);
        if (std::find(it->second.begin(), it->second.end(), child) == it->second.end()) it->second.push_back(child);
    }

    std::size_t queued = 0;
    for (std::uint64_t parentPos : parentOrder) {
        // stop if there are already a bunch of updates queued
        if (updatingCount() > maxTasks_ || executor_.queueSize() > maxTasks_) break;
        // skip any already-queued positions
        if (!tryBeginPos(parentPos)) continue;

        std::vector<std::uint64_t> children = childrenByParent[parentPos];
        const bool                 accepted =
            executor_.execute([this, parentPos, children = std::move(children)] { parentTask(parentPos, children); });
        if (accepted) ++queued;
        else endPos(parentPos); // executor shut down; try again next tick
    }
    return queued;
}

void UpdatePropagator::parentTask(std::uint64_t parentPos, const std::vector<std::uint64_t>& childPositions) {
    // Locking the parent before the children prevents deadlocks. tryLock instead of lock so this
    // worker can handle a different update (the flags stay set; the next tick retries).
    std::optional<KeyedLock<std::uint64_t>::Guard> parentGuard = updater_.locks().tryLock(parentPos);
    if (!parentGuard) {
        endPos(parentPos);
        return;
    }

    bool                            notify = false;
    std::shared_ptr<FullDataSource> parent = provider_.loadFresh(parentPos);
    if (parent) {
        bool parentUpdated = false;

        // apply each child pos to the parent
        for (std::uint64_t childPos : childPositions) {
            auto childGuard = updater_.locks().lock(childPos);

            std::shared_ptr<const FullDataSource> child = provider_.get(childPos);
            if (child) {
                std::optional<bool> changed = updateFromDataSource(*parent, *child, provider_.options().update);
                if (!changed.has_value()) {
                    logErr(
                        "Parent propagation: unsupported detail combination",
                        parentPos,
                        Error(ErrorCode::Unsupported, "detail")
                    );
                } else if (*changed) {
                    parentUpdated = true;
                }
            }

            // Cleared even when the child could not be read/merged (Java finally block).
            if (Status st = provider_.setFlag(childPos, StoreFlag::ApplyToParent, false); !st) {
                logErr("Unable to clear ApplyToParent", childPos, st.error());
            }
        }

        // If nothing changed we do not propagate further up: the nodes above need no change either.
        if (parentUpdated) {
            // propagating up, the parent will need changes
            parent->applyToParent = sec::detail(parentPos) < ROOT_SECTION_DETAIL_LEVEL;
            // only leaf nodes will ever need regenerating
            parent->regenerateLeaf = false;
            // don't modify other update propagator flags
            parent->applyToChildren = std::nullopt;

            Result<bool> r = updater_.updateLocked(*parent);
            if (!r) logErr("Parent update failed", parentPos, r.error());
            else if (*r) notify = true;
        }
    }

    parentGuard.reset();
    // listeners run without any position lock held
    if (notify) updater_.notifyListeners(parentPos);
    endPos(parentPos);
}

//===============//
// child updates //
//===============//

std::size_t UpdatePropagator::runChildUpdates(BlockPos2D target) {
    if (provider_.isClosed()) return 0;

    // Note `<` here vs `>` in the parent pass: kept from Java.
    if (!(executor_.queueSize() < maxTasks_ && updatingCount() < maxTasks_)) return 0;

    auto parentList = provider_.store().positionsToPropagateDown(target.x, target.z, maxTasks_);
    if (!parentList) {
        log::logf(LogLevel::Error, "positionsToPropagateDown failed: %s", parentList.error().message.c_str());
        return 0;
    }

    std::size_t queued = 0;
    for (std::uint64_t parentPos : *parentList) {
        if (updatingCount() > maxTasks_ || executor_.queueSize() > maxTasks_) break;
        if (!tryBeginPos(parentPos)) continue;

        if (executor_.execute([this, parentPos] { childTask(parentPos); })) ++queued;
        else endPos(parentPos);
    }
    return queued;
}

void UpdatePropagator::childTask(std::uint64_t parentPos) {
    // Don't downsample extremely large LODs: slow and takes a lot of disk (they can be generated).
    if (sec::detail(parentPos) >= HIGHEST_DOWNSAMPLE_DETAIL_LEVEL) {
        if (Status st = provider_.setFlag(parentPos, StoreFlag::ApplyToChildren, false); !st) {
            logErr("Unable to clear ApplyToChildren", parentPos, st.error());
        }
        endPos(parentPos);
        return;
    }

    std::optional<KeyedLock<std::uint64_t>::Guard> parentGuard = updater_.locks().tryLock(parentPos);
    if (!parentGuard) {
        endPos(parentPos);
        return;
    }

    std::vector<std::uint64_t>            notifyPositions;
    std::shared_ptr<const FullDataSource> parent = provider_.get(parentPos);
    if (parent) {
        // apply the parent to each child
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t childPos   = sec::child(parentPos, i);
            auto                childGuard = updater_.locks().lock(childPos);

            std::shared_ptr<FullDataSource> child = provider_.loadFresh(childPos);
            if (child) {
                std::optional<bool> changed = updateFromDataSource(*child, *parent, provider_.options().update);
                if (!changed.has_value()) {
                    logErr(
                        "Child propagation: unsupported detail combination",
                        childPos,
                        Error(ErrorCode::Unsupported, "detail")
                    );
                } else if (*changed) {
                    // don't propagate down if the child didn't change (it was probably already higher detail)
                    if (sec::detail(childPos) > LEAF_SECTION_DETAIL_LEVEL) {
                        // downsample non-leaf nodes
                        child->applyToChildren = true;
                        child->regenerateLeaf  = false;
                    } else {
                        // generate leaf nodes
                        child->applyToChildren = false;
                        child->regenerateLeaf  = true;
                    }
                    // don't modify other propagator flags
                    child->applyToParent = std::nullopt;

                    Result<bool> r = updater_.updateLocked(*child);
                    if (!r) logErr("Child update failed", childPos, r.error());
                    else if (*r) notifyPositions.push_back(childPos);
                }
            }

            // Java clears this inside every child's finally block.
            if (Status st = provider_.setFlag(parentPos, StoreFlag::ApplyToChildren, false); !st) {
                logErr("Unable to clear ApplyToChildren", parentPos, st.error());
            }
        }
    }

    parentGuard.reset();
    for (std::uint64_t p : notifyPositions) updater_.notifyListeners(p);
    endPos(parentPos);
}

} // namespace dh
