#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dh/fulldata/FullDataSource.h"
#include "dh/gen/FakeWorldGenerator.h"
#include "dh/gen/GenQueue.h"
#include "dh/gen/RetrievalQueue.h"
#include "dh/platform/IClock.h"
#include "dh/pos/SectionPos.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/util/Assert.h"
#include "dh/util/Bits.h"

using namespace dh;
using Kind = RetrievalResult::Kind;

namespace {

/// Everything a GenQueue test needs, fully deterministic: the queue loop runs only
/// when `queueExec` is driven, generation only when `pool` is driven.
struct Harness {
    FakeWorldGenerator gen;
    ManualExecutor     queueExec;
    ManualExecutor     pool;
    ManualClock        clock;
    GenQueue           queue;

    explicit Harness(int numThreads = 1) : queue(gen, queueExec, pool, numThreads, clock) {}

    /// Runs queue-loop tasks and generator tasks until both executors are idle.
    std::size_t pump(std::size_t maxRounds = 100'000) {
        std::size_t total = 0;
        for (std::size_t round = 0; round < maxRounds; ++round) {
            const std::size_t n = queueExec.runAll() + pool.runAll();
            if (n == 0) break;
            total += n;
        }
        return total;
    }

    /// One generator "tick": the pool tasks that were pending, then the resulting queue-loop work.
    void tick() {
        pool.runPending();
        queueExec.runAll();
    }
};

std::uint64_t S(int detail, int x, int z) { return sec::encode(static_cast<std::int8_t>(detail), x, z); }

Kind kindOf(const std::shared_ptr<RetrievalFuture>& f) {
    REQUIRE(f->isDone());
    return f->get()->kind;
}

std::vector<std::uint64_t> calledPositions(const FakeWorldGenerator& gen) {
    std::vector<std::uint64_t> out;
    for (const auto& c : gen.calls) out.push_back(c.request.sectionPos);
    return out;
}

struct CapturedAssert {
    static inline int count = 0;
    static void       handler(const char*, const char*, const char*, int) { ++count; }
};

} // namespace

//================//
// RetrievalFuture //
//================//

TEST_CASE("RetrievalFuture: first completion wins, callbacks fire exactly once") {
    auto f = std::make_shared<RetrievalFuture>();
    CHECK_FALSE(f->isDone());
    CHECK_FALSE(f->isCancelled());
    CHECK_FALSE(f->get().has_value());

    int before = 0, after = 0;
    f->onComplete([&](const RetrievalResult& r) {
        ++before;
        CHECK(r.kind == Kind::Split);
    });
    CHECK(before == 0);

    CHECK(f->complete(RetrievalResult::split()));
    CHECK(before == 1);
    CHECK(f->isDone());
    CHECK(kindOf(f) == Kind::Split);

    CHECK_FALSE(f->complete(RetrievalResult::failed("late")));
    CHECK_FALSE(f->cancel());
    CHECK(kindOf(f) == Kind::Split);
    CHECK_FALSE(f->isCancelled());
    CHECK(before == 1);

    // registering after completion runs inline, once
    f->onComplete([&](const RetrievalResult&) { ++after; });
    CHECK(after == 1);
}

TEST_CASE("RetrievalFuture: cancel completes with Rejected and sets isCancelled") {
    RetrievalFuture f;
    CHECK(f.cancel());
    CHECK(f.isDone());
    CHECK(f.isCancelled());
    CHECK(f.get()->kind == Kind::Rejected);
    CHECK(f.get()->error == "cancelled");
    CHECK(f.get()->source == nullptr);
}

TEST_CASE("RetrievalFuture: a callback may re-enter the future") {
    auto f     = std::make_shared<RetrievalFuture>();
    int  inner = 0;
    f->onComplete([&](const RetrievalResult&) {
        CHECK(f->isDone());
        f->onComplete([&](const RetrievalResult&) { ++inner; });
    });
    f->complete(RetrievalResult::split());
    CHECK(inner == 1);
}

//==========//
// ordering //
//==========//

TEST_CASE("taskDistance: Chebyshev distance of the section centre divided by the detail level (Java :302-303)") {
    // detail 6 section (0,0): centre block = 0*64 + 32 = (32, 32)
    CHECK(sec::centerBlockPos(S(6, 0, 0)) == BlockPos2D{32, 32});
    CHECK(GenQueue::taskDistance(S(6, 0, 0), BlockPos2D{0, 0}) == jdiv(32, 6)); // 5
    CHECK(GenQueue::taskDistance(S(6, 0, 0), BlockPos2D{32, 32}) == 0);
    // chebyshev = max(|dx|, |dz|)
    CHECK(GenQueue::taskDistance(S(6, 0, 0), BlockPos2D{32 + 100, 32 - 7}) == jdiv(100, 6)); // 16

    // detail 8 section (0,0): centre = 128; from (0,0) the chebyshev distance is 128 -> /8 = 16
    CHECK(GenQueue::taskDistance(S(8, 0, 0), BlockPos2D{0, 0}) == 16);
    // the division favours low detail: a detail-8 section 1000 blocks away (125) beats a
    // detail-6 section 800 blocks away (133)
    CHECK(GenQueue::taskDistance(S(8, 0, 0), BlockPos2D{128 + 1000, 128}) == 125);
    CHECK(GenQueue::taskDistance(S(6, 0, 0), BlockPos2D{32 + 800, 32}) == 133);

    // truncating integer division (Java `/` on int)
    CHECK(GenQueue::taskDistance(S(7, 0, 0), BlockPos2D{64 + 13, 64}) == 1); // 13 / 7
}

TEST_CASE("GenQueue: tasks start nearest-first, low detail first, ties by smallest pos") {
    Harness h(1000); // never busy by thread count

    // target at the centre of detail-6 section (0,0)
    const BlockPos2D target{32, 32};

    // distances from (32,32): S(6,0,0)=0, S(6,1,0)=64/6=10, S(6,2,0)=128/6=21, S(6,0,2)=21 (tie),
    // S(7,3,0): centre 3*128+64=448 -> 416/7=59, S(10,0,0): centre 512 -> 480/10=48
    const std::vector<std::uint64_t> submitted = {
        S(6, 2, 0),
        S(7, 3, 0),
        S(6, 0, 2),
        S(6, 1, 0),
        S(10, 0, 0),
        S(6, 0, 0),
    };
    std::vector<std::shared_ptr<RetrievalFuture>> futures;
    for (std::uint64_t pos : submitted) futures.push_back(h.queue.submit(pos, 0));
    CHECK(h.queue.waitingTaskCount() == submitted.size());
    CHECK(h.gen.calls.empty()); // submit never kicks the loop (Java)

    h.queue.startAndSetTargetPos(target);
    CHECK(h.gen.calls.empty()); // the loop is posted to the queue thread, not run inline
    h.queueExec.runAll();
    CHECK(h.gen.preStartCount == 1);

    // tie between S(6,2,0) and S(6,0,2) at distance 21: smaller encoded pos first.
    const std::uint64_t              tieFirst  = std::min(S(6, 2, 0), S(6, 0, 2));
    const std::uint64_t              tieSecond = std::max(S(6, 2, 0), S(6, 0, 2));
    const std::vector<std::uint64_t> expected  = {S(6, 0, 0), S(6, 1, 0), tieFirst, tieSecond, S(10, 0, 0), S(7, 3, 0)};
    CHECK(calledPositions(h.gen) == expected);
    CHECK(h.queue.waitingTaskCount() == 0);
    CHECK(h.queue.inProgressTaskCount() == submitted.size());

    for (const auto& c : h.gen.calls) {
        CHECK(c.request.dataDetail == sec::detail(c.request.sectionPos) - sec::SECTION_MINIMUM_DETAIL_LEVEL);
        CHECK(c.request.widthChunks == sec::blockWidth(c.request.sectionPos) / 16);
        CHECK(c.request.minChunkX == jshr32(sec::minCornerBlockX(c.request.sectionPos), 4));
        CHECK(c.request.minChunkZ == jshr32(sec::minCornerBlockZ(c.request.sectionPos), 4));
        CHECK(c.request.mode == GenerationMode::Features);
    }

    h.pump();
    for (std::size_t i = 0; i < futures.size(); ++i) {
        REQUIRE(futures[i]->isDone());
        CHECK(futures[i]->get()->kind == Kind::Success);
        CHECK(futures[i]->get()->pos == submitted[i]);
        REQUIRE(futures[i]->get()->source != nullptr);
        CHECK(futures[i]->get()->source->pos == submitted[i]);
    }
    CHECK(h.queue.inProgressTaskCount() == 0);
}

TEST_CASE("GenQueue: moving the target re-prioritises the waiting tasks") {
    Harness h(1);
    h.gen.busy = true; // hold everything back while we set up

    const std::uint64_t west = S(6, -10, 0), east = S(6, 10, 0), mid = S(6, 0, 0);
    auto                fw = h.queue.submit(west, 0);
    auto                fe = h.queue.submit(east, 0);
    auto                fm = h.queue.submit(mid, 0);

    h.queue.startAndSetTargetPos(BlockPos2D{-640, 32}); // next to `west`
    h.queueExec.runAll();
    CHECK(h.gen.calls.empty()); // generator busy
    CHECK(h.gen.preStartCount == 1);
    CHECK(h.queue.waitingTaskCount() == 3);

    h.gen.busy = false;
    h.queue.startAndSetTargetPos(BlockPos2D{640, 32}); // now next to `east`
    h.queueExec.runAll();
    // numThreads = 1: busy once inProgress > 1, so exactly two tasks start, east first
    REQUIRE(h.gen.calls.size() == 2);
    CHECK(h.gen.calls[0].request.sectionPos == east);
    CHECK(h.gen.calls[1].request.sectionPos == mid);
    CHECK(h.queue.waitingTaskCount() == 1);
    CHECK(h.queue.isGeneratorBusy());

    h.pump();
    REQUIRE(h.gen.calls.size() == 3);
    CHECK(h.gen.calls[2].request.sectionPos == west);
    CHECK(kindOf(fw) == Kind::Success);
    CHECK(kindOf(fe) == Kind::Success);
    CHECK(kindOf(fm) == Kind::Success);
}

//=================//
// split / reject  //
//=================//

TEST_CASE("GenQueue: a detail level the generator cannot produce completes with Split") {
    Harness h(4);
    h.gen.largest = 2; // can generate section detail 6..8 only

    auto tooBig = h.queue.submit(S(9, 0, 0), 0); // requested data detail 3 > 2
    auto okay   = h.queue.submit(S(8, 0, 0), 0); // requested data detail 2
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.pump();

    REQUIRE(tooBig->isDone());
    CHECK(tooBig->get()->kind == Kind::Split);
    CHECK(tooBig->get()->source == nullptr);
    CHECK(tooBig->get()->pos == 0); // Java CreateSplit(): pos 0, no data source
    CHECK_FALSE(tooBig->isCancelled());

    CHECK(kindOf(okay) == Kind::Success);
    REQUIRE(h.gen.calls.size() == 1);
    CHECK(h.gen.calls[0].request.sectionPos == S(8, 0, 0));
    CHECK(h.gen.calls[0].request.dataDetail == 2);
}

TEST_CASE("GenQueue: submit clamps the required detail to the generator's largest and rejects below its smallest") {
    Harness h(4);
    h.gen.smallest = 2;
    h.gen.largest  = 4;

    // below the generator's smallest: Java throws UnsupportedOperationException
    auto tooFine = h.queue.submit(S(10, 0, 0), 1);
    REQUIRE(tooFine->isDone());
    CHECK(tooFine->get()->kind == Kind::Rejected);
    CHECK(h.queue.waitingTaskCount() == 0);

    // above the largest: clamped (Java :152-155), then generated
    auto clamped = h.queue.submit(S(10, 0, 0), 9);
    CHECK_FALSE(clamped->isDone());
    CHECK(h.queue.waitingTaskCount() == 1);
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.pump();
    CHECK(kindOf(clamped) == Kind::Success);
}

TEST_CASE("GenQueue: a request smaller than a chunk is an assertion failure and is rejected") {
    Harness       h(4);
    AssertHandler previous = setAssertHandler(&CapturedAssert::handler);
    CapturedAssert::count  = 0;

    // detail 6 section with required data detail 2: 6 > 2 + 4 is false (Java :158)
    auto f = h.queue.submit(S(6, 0, 0), 2);
    CHECK(CapturedAssert::count == 1);
    REQUIRE(f->isDone());
    CHECK(f->get()->kind == Kind::Rejected);
    CHECK(h.queue.waitingTaskCount() == 0);

    setAssertHandler(previous);
}

//=================//
// duplicate merge //
//=================//

TEST_CASE("GenQueue: submitting a waiting pos again returns the same future") {
    Harness h(4);
    auto    a = h.queue.submit(S(6, 1, 1), 0);
    auto    b = h.queue.submit(S(6, 1, 1), 0);
    CHECK(a == b);
    CHECK(h.queue.waitingTaskCount() == 1);

    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.pump();
    CHECK(h.gen.calls.size() == 1);
    CHECK(kindOf(a) == Kind::Success);
}

TEST_CASE("GenQueue: submitting an in-progress pos merges into the running task (Java :327-354)") {
    Harness h(4);
    h.gen.script(3); // keep the first task running for a few ticks

    const std::uint64_t pos   = S(6, 1, 1);
    auto                first = h.queue.submit(pos, 0);
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    REQUIRE(h.gen.calls.size() == 1);
    CHECK(h.queue.inProgressTaskCount() == 1);

    // the pos is no longer waiting, so a new task is created ...
    auto second = h.queue.submit(pos, 0);
    CHECK(second != first);
    CHECK(h.queue.waitingTaskCount() == 1);

    // ... and when it is picked it is chained to the running one instead of starting again
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    CHECK(h.gen.calls.size() == 1);
    CHECK(h.queue.waitingTaskCount() == 0);
    CHECK(h.queue.inProgressTaskCount() == 1);
    CHECK_FALSE(first->isDone());
    CHECK_FALSE(second->isDone());

    int firstCallbacks = 0, secondCallbacks = 0;
    first->onComplete([&](const RetrievalResult&) { ++firstCallbacks; });
    second->onComplete([&](const RetrievalResult&) { ++secondCallbacks; });

    h.pump();
    REQUIRE(kindOf(first) == Kind::Success);
    REQUIRE(kindOf(second) == Kind::Success);
    CHECK(first->get()->source == second->get()->source); // the very same data source
    CHECK(first->get()->pos == pos);
    CHECK(second->get()->pos == pos);
    CHECK(firstCallbacks == 1);
    CHECK(secondCallbacks == 1);
    CHECK(h.queue.inProgressTaskCount() == 0);
    CHECK(h.gen.calls.size() == 1);
}

TEST_CASE("GenQueue: a merged duplicate of a task that fails also fails (Java :349-353)") {
    Harness h(4);
    h.gen.script(2, true);

    const std::uint64_t pos   = S(6, 1, 1);
    auto                first = h.queue.submit(pos, 0);
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    auto second = h.queue.submit(pos, 0);
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.pump();

    CHECK(kindOf(first) == Kind::Failed);
    CHECK(kindOf(second) == Kind::Failed);
    CHECK(second->get()->error == "scripted failure");
    CHECK(h.gen.calls.size() == 1);
    CHECK(h.queue.inProgressTaskCount() == 0);
}

//=========//
// failure //
//=========//

TEST_CASE("GenQueue: a failed task frees its slot and the next task starts (Java :400-402)") {
    Harness h(1);          // cap: inProgress > 1 is busy -> at most 2 running
    h.gen.script(0, true); // first started task fails
    h.gen.script(0, false);
    h.gen.script(0, false);

    auto a = h.queue.submit(S(6, 0, 0), 0); // nearest -> fails
    auto b = h.queue.submit(S(6, 1, 0), 0);
    auto c = h.queue.submit(S(6, 2, 0), 0);
    h.queue.startAndSetTargetPos(BlockPos2D{32, 32});
    h.queueExec.runAll();
    REQUIRE(h.gen.calls.size() == 2);
    CHECK(h.queue.inProgressTaskCount() == 2);
    CHECK(h.queue.isGeneratorBusy());

    h.tick(); // both complete (one failing); the handlers restart the loop which starts `c`
    REQUIRE(kindOf(a) == Kind::Failed);
    CHECK(a->get()->error == "scripted failure");
    CHECK(a->get()->source == nullptr);
    CHECK(kindOf(b) == Kind::Success);
    REQUIRE(h.gen.calls.size() == 3);
    CHECK(h.gen.calls[2].request.sectionPos == S(6, 2, 0));
    CHECK(h.queue.inProgressTaskCount() == 1);

    h.pump();
    CHECK(kindOf(c) == Kind::Success);
    CHECK(h.queue.inProgressTaskCount() == 0);
}

TEST_CASE("GenQueue: the rolling chunk-time average is fed from the clock (Java :388-394, RollingAverage quirk)") {
    Harness h(4);
    h.gen.script(1);
    auto f = h.queue.submit(S(6, 0, 0), 0); // 64 blocks = 4x4 chunks = 16 chunks
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    h.clock.advanceMillis(160);
    h.pump();
    CHECK(kindOf(f) == Kind::Success);
    // 160 ms / 16 chunks = 10 ms per chunk; Java's RollingAverage divides the first
    // sample by currentSize = max(index + 1, currentSize) = 2.
    CHECK(h.queue.averageChunkGenTimeMs() == doctest::Approx(5.0));
}

//===========//
// busy gate //
//===========//

TEST_CASE("GenQueue: nothing starts while the generator is busy; the thread cap is inProgress > max(threads, 1)") {
    Harness h(2); // busy once inProgress > 2, i.e. at most 3 running
    for (int i = 0; i < 6; ++i) h.queue.submit(S(6, i, 0), 0);
    h.gen.defaultScript.delayTicks = 2;

    h.gen.busy = true;
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    CHECK(h.gen.calls.empty());
    CHECK(h.gen.preStartCount == 1);
    CHECK(h.queue.isGeneratorBusy());

    h.gen.busy = false;
    CHECK_FALSE(h.queue.isGeneratorBusy());
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
    h.queueExec.runAll();
    CHECK(h.gen.calls.size() == 3);
    CHECK(h.queue.inProgressTaskCount() == 3);
    CHECK(h.queue.isGeneratorBusy());
    CHECK(h.queue.waitingTaskCount() == 3);

    // ticks that do not complete anything do not start anything either
    h.tick();
    CHECK(h.gen.calls.size() == 3);
    h.tick();
    CHECK(h.gen.calls.size() == 3);
    h.tick(); // first batch completes -> handlers restart the loop -> next 3 start
    CHECK(h.gen.calls.size() == 6);
    CHECK(h.queue.inProgressTaskCount() == 3);
    CHECK(h.queue.waitingTaskCount() == 0);

    // numThreads <= 0 behaves like 1 (Math.max(threads, 1))
    h.queue.setNumThreads(0);
    CHECK(h.queue.isGeneratorBusy()); // 3 > 1
    h.pump();
    CHECK(h.queue.inProgressTaskCount() == 0);
}

//======================//
// cancellation / close //
//======================//

TEST_CASE("GenQueue: removeRetrievalRequestIf cancels matching waiting tasks only") {
    Harness h(4);
    h.gen.script(5);
    auto running = h.queue.submit(S(6, 0, 0), 0);
    h.queue.startAndSetTargetPos(BlockPos2D{32, 32});
    h.queueExec.runAll();
    REQUIRE(h.queue.inProgressTaskCount() == 1);

    auto keep   = h.queue.submit(S(6, 5, 0), 0);
    auto remove = h.queue.submit(S(6, 0, 5), 0);
    CHECK(h.queue.waitingTaskCount() == 2);

    std::vector<std::uint64_t> seen;
    h.queue.removeRetrievalRequestIf([&](std::uint64_t pos) {
        seen.push_back(pos);
        return sec::z(pos) == 5;
    });
    // the predicate sees only waiting tasks, in ascending pos order
    std::vector<std::uint64_t> expectedSeen = {S(6, 5, 0), S(6, 0, 5)};
    std::sort(expectedSeen.begin(), expectedSeen.end());
    CHECK(seen == expectedSeen);

    REQUIRE(remove->isDone());
    CHECK(remove->isCancelled());
    CHECK(remove->get()->kind == Kind::Rejected);
    CHECK_FALSE(keep->isDone());
    CHECK_FALSE(running->isDone());
    CHECK(h.queue.waitingTaskCount() == 1);
    CHECK(h.queue.requestPosExistsWhere([](std::uint64_t pos) { return sec::x(pos) == 5; }));
    CHECK_FALSE(h.queue.requestPosExistsWhere([](std::uint64_t pos) { return sec::z(pos) == 5; }));

    h.pump();
    CHECK(kindOf(running) == Kind::Success);
    CHECK(kindOf(keep) == Kind::Success);
    CHECK(h.gen.calls.size() == 2);
}

TEST_CASE("GenQueue: shutdown(false) cancels waiting and running futures but lets the generator finish") {
    Harness h(4);
    h.gen.script(2);
    auto running = h.queue.submit(S(6, 0, 0), 0);
    h.queue.startAndSetTargetPos(BlockPos2D{32, 32});
    h.queueExec.runAll();
    auto waiting = h.queue.submit(S(6, 9, 9), 0);
    REQUIRE(h.queue.inProgressTaskCount() == 1);
    REQUIRE(h.queue.waitingTaskCount() == 1);

    h.queue.shutdown(false);
    CHECK(h.queue.isClosing());
    CHECK(h.queue.waitingTaskCount() == 0);
    CHECK(h.queue.inProgressTaskCount() == 1); // slot is freed by the completion handler, not by shutdown
    REQUIRE(waiting->isDone());
    CHECK(waiting->isCancelled());
    REQUIRE(running->isDone());
    CHECK(running->isCancelled());
    CHECK(h.gen.closeCount == 0);
    CHECK(h.gen.inFlight() == 1);

    // new submissions are refused while closing (Java :129-134)
    auto late = h.queue.submit(S(6, 3, 3), 0);
    REQUIRE(late->isDone());
    CHECK(late->get()->kind == Kind::Rejected);
    CHECK(h.queue.waitingTaskCount() == 0);

    // the running generation completes normally; its slot is freed and the generator is closed
    h.pump();
    CHECK(h.queue.inProgressTaskCount() == 0);
    CHECK(h.gen.inFlight() == 0);
    CHECK(h.gen.closeCount == 1);
    CHECK(running->isCancelled()); // the late success does not overwrite the cancellation
    CHECK(h.gen.calls.size() == 1);

    h.queue.shutdown(true); // idempotent
    CHECK(h.gen.closeCount == 1);
}

TEST_CASE("GenQueue: shutdown(true) closes the generator immediately, aborting running work") {
    Harness h(4);
    h.gen.defaultScript.delayTicks = 5;
    auto a                         = h.queue.submit(S(6, 0, 0), 0);
    auto b                         = h.queue.submit(S(6, 1, 0), 0);
    h.queue.startAndSetTargetPos(BlockPos2D{32, 32});
    h.queueExec.runAll();
    REQUIRE(h.queue.inProgressTaskCount() == 2);

    int callbacks = 0;
    a->onComplete([&](const RetrievalResult& r) {
        ++callbacks;
        CHECK(r.kind == Kind::Rejected);
    });

    h.queue.shutdown(true);
    CHECK(h.gen.closeCount == 1);
    CHECK(h.gen.inFlight() == 0);
    CHECK(h.queue.inProgressTaskCount() == 0); // the Cancelled callbacks freed the slots synchronously
    CHECK(a->isCancelled());
    CHECK(b->isCancelled());
    CHECK(callbacks == 1);

    // stale pool tasks are no-ops and nothing restarts the loop
    h.pump();
    CHECK(h.gen.calls.size() == 2);
    CHECK(callbacks == 1);
}

TEST_CASE("GenQueue: shutdown(true) after a deferred shutdown(false) still closes the generator (Java :795)") {
    Harness h(4);
    h.gen.script(3);
    auto running = h.queue.submit(S(6, 0, 0), 0);
    h.queue.startAndSetTargetPos(BlockPos2D{32, 32});
    h.queueExec.runAll();
    REQUIRE(h.queue.inProgressTaskCount() == 1);

    h.queue.shutdown(false); // defers generator.close() until the lingering task reports back
    CHECK(running->isCancelled());
    CHECK(h.gen.closeCount == 0);
    CHECK(h.gen.inFlight() == 1);

    h.queue.shutdown(true); // must not be swallowed by the "already closing" early return
    CHECK(h.gen.closeCount == 1);
    CHECK(h.gen.inFlight() == 0);
    CHECK(h.queue.inProgressTaskCount() == 0);

    h.pump(); // stale pool tasks, nothing else
    CHECK(h.gen.closeCount == 1);
    CHECK(h.gen.calls.size() == 1);

    h.queue.shutdown(true); // and a third call is a no-op
    h.queue.shutdown(false);
    CHECK(h.gen.closeCount == 1);
}

TEST_CASE("GenQueue: destroying the queue after a deferred shutdown(false) closes the generator") {
    FakeWorldGenerator gen;
    ManualExecutor     queueExec, pool;
    ManualClock        clock;
    gen.script(3);
    {
        GenQueue queue(gen, queueExec, pool, 4, clock);
        auto     running = queue.submit(S(6, 0, 0), 0);
        queue.startAndSetTargetPos(BlockPos2D{32, 32});
        queueExec.runAll();
        REQUIRE(queue.inProgressTaskCount() == 1);
        queue.shutdown(false);
        CHECK(gen.closeCount == 0);
        CHECK(gen.inFlight() == 1);
    }
    CHECK(gen.closeCount == 1);
    CHECK(gen.inFlight() == 0);
    pool.runAll();
    queueExec.runAll();
    CHECK(gen.closeCount == 1);
}

TEST_CASE("GenQueue: shutdown(false) with nothing running closes the generator at once and stays idempotent") {
    Harness h(4);
    h.queue.shutdown(false);
    CHECK(h.gen.closeCount == 1);
    h.queue.shutdown(true);
    CHECK(h.gen.closeCount == 1);
}

TEST_CASE("GenQueue: destroying the queue shuts it down and stale executor tasks are harmless") {
    FakeWorldGenerator               gen;
    ManualExecutor                   queueExec, pool;
    ManualClock                      clock;
    std::shared_ptr<RetrievalFuture> f;
    {
        GenQueue queue(gen, queueExec, pool, 1, clock);
        gen.defaultScript.delayTicks = 3;
        f                            = queue.submit(S(6, 0, 0), 0);
        queue.startAndSetTargetPos(BlockPos2D{0, 0});
        queueExec.runAll();
        REQUIRE(queue.inProgressTaskCount() == 1);
        queue.startAndSetTargetPos(BlockPos2D{1, 1}); // leaves a loop task queued
        CHECK(queueExec.queueSize() == 1);
    }
    CHECK(gen.closeCount == 1);
    CHECK(f->isCancelled());
    // the queued loop task and the generator's pool task run against a dead queue: no-ops
    CHECK(queueExec.runAll() == 1);
    pool.runAll();
    CHECK(f->get()->kind == Kind::Rejected);
}

TEST_CASE("GenQueue: a queue thread that rejects work does not get stuck 'running'") {
    Harness h(4);
    auto    f = h.queue.submit(S(6, 0, 0), 0);
    h.queueExec.shutdown();
    h.queue.startAndSetTargetPos(BlockPos2D{0, 0}); // execute() returns false
    CHECK(h.queueExec.queueSize() == 0);
    CHECK_FALSE(f->isDone());
    h.queue.shutdown(true);
    CHECK(f->isCancelled());
}

//===============//
// property test //
//===============//

TEST_CASE("GenQueue property: every task completes exactly once, never starts while busy, respects the cap") {
    struct Tracked {
        std::uint64_t                    pos;
        std::shared_ptr<RetrievalFuture> future;
        int                              callbacks = 0;
    };

    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        std::mt19937 rng(seed);
        auto         pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint32_t>(n)); };

        const int numThreads = pick(4); // 0..3 -> cap max(n,1)+1
        const int cap        = std::max(numThreads, 1) + 1;
        Harness   h(numThreads);
        h.gen.largest = static_cast<std::int8_t>(1 + pick(4)); // section details above largest+6 split

        std::vector<std::shared_ptr<Tracked>> tracked;
        std::set<RetrievalFuture*>            distinctFutures;

        auto track = [&](std::uint64_t pos, std::shared_ptr<RetrievalFuture> f) {
            auto t    = std::make_shared<Tracked>();
            t->pos    = pos;
            t->future = f;
            tracked.push_back(t);
            std::weak_ptr<Tracked> weak = t;
            f->onComplete([weak](const RetrievalResult&) {
                if (auto s = weak.lock()) ++s->callbacks;
            });
            distinctFutures.insert(f.get());
        };

        auto checkInvariants = [&] {
            CHECK(h.queue.inProgressTaskCount() <= static_cast<std::size_t>(cap));
            for (const auto& c : h.gen.calls) CHECK_FALSE(c.busyAtCall);
        };

        const int steps = 40 + pick(120);
        for (int step = 0; step < steps; ++step) {
            switch (pick(10)) {
            case 0:
            case 1:
            case 2: { // submit (small pos pool so duplicates are common)
                const int           detail = 6 + pick(5);
                const std::uint64_t pos    = S(detail, pick(4) - 2, pick(4) - 2);
                const int           req    = pick(std::min(detail - 5, 3)); // keeps detail > req + 4
                track(pos, h.queue.submit(pos, static_cast<std::uint8_t>(req)));
                break;
            }
            case 3: // move target and (re)start the loop
                h.queue.startAndSetTargetPos(BlockPos2D{pick(600) - 300, pick(600) - 300});
                break;
            case 4: // run one queue-loop task
                h.queueExec.runOne();
                break;
            case 5:
            case 6: // run one generator task
                h.pool.runOne();
                break;
            case 7: // scripted behaviour for the next generate() call
                h.gen.script(pick(3), pick(4) == 0);
                break;
            case 8: // toggle generator busy flag
                h.gen.busy = pick(2) == 0;
                break;
            case 9: // drop some waiting tasks
                h.queue.removeRetrievalRequestIf([&](std::uint64_t p) {
                    return (sec::x(p) + sec::z(p) + pick(2)) % 3 == 0;
                });
                break;
            }
            checkInvariants();
        }

        // drain: generator available, loop restarted, everything runs to completion
        h.gen.busy = false;
        h.queue.startAndSetTargetPos(BlockPos2D{0, 0});
        h.pump();
        checkInvariants();

        CHECK(h.queue.waitingTaskCount() == 0);
        CHECK(h.queue.inProgressTaskCount() == 0);
        CHECK(h.gen.inFlight() == 0);

        std::map<RetrievalFuture*, int> callbacksPerFuture;
        for (const auto& t : tracked) {
            REQUIRE(t->future->isDone());
            const RetrievalResult r = *t->future->get();
            CHECK(t->callbacks == 1);
            callbacksPerFuture[t->future.get()] += t->callbacks;
            if (r.kind == Kind::Success) {
                REQUIRE(r.source != nullptr);
                CHECK(r.pos == t->pos);
                CHECK(r.source->pos == t->pos);
            } else if (r.kind == Kind::Split) {
                CHECK(r.source == nullptr);
                CHECK(sec::detail(t->pos) - sec::SECTION_MINIMUM_DETAIL_LEVEL > h.gen.largest);
            } else if (r.kind == Kind::Rejected) {
                CHECK(t->future->isCancelled()); // only removeRetrievalRequestIf rejects in this scenario
            } else {
                CHECK(r.kind == Kind::Failed);
            }
        }
        // a future shared by several submits still fired each registered callback exactly once
        CHECK(callbacksPerFuture.size() == distinctFutures.size());
        for (const auto& [future, n] : callbacksPerFuture) {
            int registrations = 0;
            for (const auto& t : tracked) registrations += (t->future.get() == future) ? 1 : 0;
            CHECK(n == registrations);
        }

        // every generate() call produced exactly one in-progress completion
        std::size_t generated = 0;
        for (const auto& c : h.gen.calls) generated += c.poolRejected ? 0 : 1;
        CHECK(generated == h.gen.calls.size());

        h.queue.shutdown(true);
        CHECK(h.gen.closeCount == 1);
        for (const auto& t : tracked) CHECK(t->callbacks == 1);
    }
}
