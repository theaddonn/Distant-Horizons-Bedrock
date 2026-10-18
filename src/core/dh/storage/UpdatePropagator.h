#pragma once

// Port of FullDataUpdatePropagatorV2 (file/fullDatafile/V2/FullDataUpdatePropagatorV2.java):
// pushes section changes up (child -> parent downsample) and down (parent -> child) using the
// ApplyToParent / ApplyToChildren store flags.
//
// Java ran a thread that looped every 250 ms (PROPAGATE_QUEUE_THREAD_DELAY_IN_MS). Here
// tick() is exactly one iteration of that loop body (runParentUpdates then runChildUpdates;
// the regeneration queue belongs to the generated provider, WP-J2); the caller owns the timer.
//
// Determinism: Java grouped children in HashMap<parent, HashSet<child>> and iterated hash order.
// Here parents are processed in order of their first appearance in the store's nearest-first
// list and the children of one parent in that same list order (distance, then pos).
// Deviation: a position whose execute() was rejected is released again (Java leaked it in
// updatingPosSet forever).

#include "dh/platform/IExecutor.h"
#include "dh/pos/BlockPos.h"
#include "dh/storage/SourceProvider.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

namespace dh {

class UpdatePropagator {
public:
    static constexpr int NUMBER_OF_PARENT_UPDATE_TASKS_PER_THREAD = 10;
    static constexpr int PROPAGATE_QUEUE_THREAD_DELAY_IN_MS       = 250;

    /// `threadCount` = Config.Common.MultiThreading.numberOfThreads (task cap = 10 x threads).
    UpdatePropagator(SourceProvider& provider, IExecutor& executor, int threadCount);

    struct TickStats {
        std::size_t parentTasks = 0; ///< tasks handed to the executor by runParentUpdates
        std::size_t childTasks  = 0; ///< ... by runChildUpdates
    };

    /// One loop iteration; `target` = player block position (BlockPos2D{0,0} when none).
    TickStats tick(BlockPos2D target);

    /// Java runParentUpdates / runChildUpdates (bottom -> up / top -> down).
    std::size_t runParentUpdates(BlockPos2D target);
    std::size_t runChildUpdates(BlockPos2D target);

    std::size_t maxTaskCount() const { return maxTasks_; }
    std::size_t updatingCount() const;

private:
    bool tryBeginPos(std::uint64_t pos);
    void endPos(std::uint64_t pos);

    void parentTask(std::uint64_t parentPos, const std::vector<std::uint64_t>& childPositions);
    void childTask(std::uint64_t parentPos);

    SourceProvider& provider_;
    SourceUpdater&  updater_;
    IExecutor&      executor_;
    std::size_t     maxTasks_;

    mutable std::mutex      updatingMutex_;
    std::set<std::uint64_t> updating_; ///< Java updatingPosSet
};

} // namespace dh
