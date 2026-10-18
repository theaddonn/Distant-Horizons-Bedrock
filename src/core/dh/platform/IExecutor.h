#pragma once

// Task-execution seam (Java: java.util.concurrent.Executor / PriorityTaskPicker.Executor).
//
// Semantics every implementation must honour:
//   * execute() returns false iff the task was rejected (executor shut down).
//     The task is then dropped, never run. Java threw RejectedExecutionException;
//     DH callers treated that as a quiet no-op, so here it is just `false`.
//   * Tasks must not throw (there are no exceptions in the core).
//   * Tasks may be run on any thread, possibly inline inside execute()
//     (InlineExecutor). Code that needs a specific thread must not assume one.
//   * queueSize() is advisory (used for back-pressure and the debug HUD) and may
//     be stale by the time the caller reads it.
//
// Implementations: WP-H ThreadPools (real pools), thread/ManualExecutor.h and
// thread/InlineExecutor.h (deterministic test doubles), glue LeviExecutor
// (marshals onto the Bedrock main / render thread).

#include <cstddef>
#include <functional>

namespace dh {

struct IExecutor {
    using Task = std::function<void()>;

    virtual ~IExecutor() = default;

    /// Queues `task`. Returns false if rejected (executor shut down); the task is not run.
    virtual bool execute(Task task) = 0;

    /// Number of tasks waiting to run (not counting the ones currently running).
    virtual std::size_t queueSize() const = 0;
};

} // namespace dh
