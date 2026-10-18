#pragma once

// Scripted IWorldGenerator test double for GenQueue tests.
//
// Every generate() call consumes the next Script (or `defaultScript` when the
// script queue is empty) and posts a self-rescheduling job onto the gen pool:
// the job completes when its (delayTicks + 1)-th pool task runs. Driving the
// pool with ManualExecutor::runPending() once per "tick" therefore makes a
// delay-d job finish on tick d + 1, deterministically and without sleeps.
//
// close() cancels every outstanding job synchronously (onDone fires with
// ErrorCode::Cancelled before close() returns) and makes their stale pool tasks
// no-ops, honouring the IWorldGenerator contract.

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dh/fulldata/FullDataSource.h"
#include "dh/gen/IWorldGenerator.h"
#include "dh/platform/IExecutor.h"

namespace dh {

class FakeWorldGenerator final : public IWorldGenerator {
public:
    struct Script {
        int         delayTicks = 0;     ///< extra pool tasks before completion
        bool        fail       = false; ///< complete with ErrorCode::Internal instead of a data source
        std::string error      = "scripted failure";
    };

    struct Call {
        GenerateRequest request;
        bool            busyAtCall   = false; ///< value of `busy` when generate() was called
        bool            poolRejected = false;
    };

    // --- knobs the test sets ---------------------------------------------------
    std::int8_t        smallest = 0;
    std::int8_t        largest  = 12;
    bool               busy     = false;
    Script             defaultScript{};
    std::deque<Script> scripts; ///< consumed front-first by generate()

    // --- observations -----------------------------------------------------------
    std::vector<Call> calls;
    int               preStartCount = 0;
    int               closeCount    = 0;

    void script(Script s) { scripts.push_back(std::move(s)); }
    void script(int delayTicks, bool fail = false) { scripts.push_back(Script{delayTicks, fail, "scripted failure"}); }

    /// Jobs started but not yet completed/cancelled.
    std::size_t inFlight() const {
        std::lock_guard lock(mutex_);
        return jobs_.size();
    }
    bool isClosed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    // --- IWorldGenerator ---------------------------------------------------------
    std::int8_t smallestDataDetailLevel() const override { return smallest; }
    std::int8_t largestDataDetailLevel() const override { return largest; }
    bool        isBusy() const override { return busy; }
    void        preGeneratorTaskStart() override { ++preStartCount; }

    void generate(const GenerateRequest& request, IExecutor& genPool, ChunkCallback, DoneCallback onDone) override {
        Call call;
        call.request    = request;
        call.busyAtCall = busy;

        Script s = defaultScript;
        if (!scripts.empty()) {
            s = scripts.front();
            scripts.pop_front();
        }

        auto job            = std::make_shared<Job>();
        job->request        = request;
        job->onDone         = std::move(onDone);
        job->remainingTicks = s.delayTicks;
        job->fail           = s.fail;
        job->error          = s.error;

        bool closedNow;
        {
            std::lock_guard lock(mutex_);
            closedNow = closed_;
            if (!closedNow) jobs_.push_back(job);
        }
        if (closedNow) {
            calls.push_back(call);
            if (job->onDone) job->onDone(Error{ErrorCode::Cancelled, "generator closed"});
            return;
        }

        const bool accepted = genPool.execute([this, job, pool = &genPool] { step(job, *pool); });
        call.poolRejected   = !accepted;
        calls.push_back(call);
        if (!accepted) finish(job, Error{ErrorCode::Rejected, "gen pool rejected task"});
    }

    void close() override {
        std::vector<std::shared_ptr<Job>> pending;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            pending.swap(jobs_);
        }
        ++closeCount;
        for (auto& job : pending) {
            DoneCallback cb;
            {
                std::lock_guard lock(mutex_);
                if (job->done) continue;
                job->done = true;
                cb        = std::move(job->onDone);
            }
            if (cb) cb(Error{ErrorCode::Cancelled, "generator closed"});
        }
    }

private:
    struct Job {
        GenerateRequest request;
        DoneCallback    onDone;
        int             remainingTicks = 0;
        bool            fail           = false;
        bool            done           = false;
        std::string     error;
    };

    void step(const std::shared_ptr<Job>& job, IExecutor& pool) {
        bool reschedule = false;
        {
            std::lock_guard lock(mutex_);
            if (job->done) return; // cancelled by close()
            if (job->remainingTicks > 0) {
                --job->remainingTicks;
                reschedule = true;
            }
        }
        if (reschedule) {
            if (!pool.execute([this, job, p = &pool] { step(job, *p); })) {
                finish(job, Error{ErrorCode::Rejected, "gen pool rejected task"});
            }
            return;
        }
        if (job->fail) {
            finish(job, Error{ErrorCode::Internal, job->error});
        } else {
            finish(job, std::make_shared<FullDataSource>(FullDataSource::createEmpty(job->request.sectionPos)));
        }
    }

    void finish(const std::shared_ptr<Job>& job, Result<std::shared_ptr<FullDataSource>> result) {
        DoneCallback cb;
        {
            std::lock_guard lock(mutex_);
            if (job->done) return;
            job->done = true;
            cb        = std::move(job->onDone);
            for (std::size_t i = 0; i < jobs_.size(); ++i) {
                if (jobs_[i] == job) {
                    jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (cb) cb(std::move(result));
    }

    mutable std::mutex                mutex_;
    std::vector<std::shared_ptr<Job>> jobs_;
    bool                              closed_ = false;
};

} // namespace dh
