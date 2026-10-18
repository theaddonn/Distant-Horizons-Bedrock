#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "dh/build/LightingEngine.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/fulldata/Merge.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/ChunkHashStore.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/storage/SourceProvider.h"
#include "dh/storage/UpdatePropagator.h"
#include "dh/thread/ManualExecutor.h"
#include "golden_loader.h"

using namespace dh;

namespace {

struct Fx {
    MapBlockRegistry                reg{false};
    ManualClock                     clock;
    ManualExecutor                  fileExec;
    ManualExecutor                  propExec;
    MemoryLodStore                  store;
    const BlockInfo*                stone = nullptr;
    const BlockInfo*                dirt  = nullptr;
    std::unique_ptr<SourceProvider> provider;

    explicit Fx(int maxSky = 15) {
        BlockInfo s;
        s.serial  = "minecraft:stone";
        s.opacity = 16;
        stone     = reg.addBlock(std::move(s));
        BlockInfo d;
        d.serial  = "minecraft:dirt";
        d.opacity = 16;
        dirt      = reg.addBlock(std::move(d));
        ProviderOptions po;
        po.maxSkyLight = maxSky;
        provider       = std::make_unique<SourceProvider>(store, reg, clock, fileExec, CodecOptions{}, po);
    }

    /// Section with `cols` columns filled (first `cols` in index order), one block each.
    std::shared_ptr<FullDataSource> make(
        int              detail,
        int              x,
        int              z,
        const BlockInfo* block,
        int              height,
        int              sky  = 0,
        int              cols = FullDataSource::COLUMN_COUNT
    ) {
        auto s = std::make_shared<FullDataSource>(
            FullDataSource::createEmpty(sec::encode(static_cast<std::int8_t>(detail), x, z))
        );
        int id = s->mapping.addIfAbsent(reg.defaultBiome(), block);
        for (int i = 0; i < cols; ++i) {
            // full section, or a 2-wide strip (cx = 0..1) so every 2x2 downsample group is fully covered
            const bool full = cols == FullDataSource::COLUMN_COUNT;
            const int  cx   = full ? i / FullDataSource::WIDTH : i % 2;
            const int  cz   = full ? i % FullDataSource::WIDTH : i / 2;
            s->setSingleColumn({fdp::encode(id, height, 60, 0, static_cast<std::uint8_t>(sky))}, cx, cz, 5, 0);
        }
        s->isEmpty = false;
        if (detail < ROOT_SECTION_DETAIL_LEVEL) s->applyToParent = true;
        return s;
    }

    SourceUpdater& up() { return provider->updater(); }
};

constexpr BlockPos2D kOrigin{0, 0};

/// tick + drain until the propagator has nothing left to do.
int settle(Fx& f, UpdatePropagator& prop, int maxRounds = 200) {
    int rounds = 0;
    for (; rounds < maxRounds; ++rounds) {
        auto        stats = prop.tick(kOrigin);
        std::size_t ran   = f.propExec.runAll();
        auto        up    = f.store.positionsToPropagateUp(0, 0, 1000);
        auto        down  = f.store.positionsToPropagateDown(0, 0, 1000);
        if (stats.parentTasks == 0 && stats.childTasks == 0 && ran == 0 && up->empty() && down->empty()) break;
    }
    return rounds;
}

} // namespace

TEST_CASE("provider: missing -> createEmpty, cached identity, update replaces snapshot") {
    Fx   f;
    auto pos = sec::encode(6, 3, -2);

    auto a = f.provider->get(pos);
    REQUIRE(a);
    CHECK(a->isEmpty);
    CHECK(a->pos == pos);
    auto b = f.provider->get(pos);
    CHECK(a.get() == b.get());  // shared snapshot
    CHECK(f.store.size() == 0); // createEmpty is not persisted

    auto in = f.make(6, 3, -2, f.stone, 10, 3, 8);
    auto r  = f.up().update(in);
    REQUIRE(r);
    CHECK(*r);
    CHECK(f.store.size() == 1);

    auto c = f.provider->get(pos);
    REQUIRE(c);
    CHECK(c.get() != a.get());
    CHECK_FALSE(c->isEmpty);
    CHECK(c.get() == f.provider->get(pos).get());

    // survives eviction of the cache: reload from the store equals the snapshot
    a.reset();
    b.reset();
    f.provider->invalidate(pos);
    auto d = f.provider->get(pos);
    REQUIRE(d);
    CHECK(d.get() != c.get());
    CHECK(d->contentEquals(*c));
}

TEST_CASE("provider: weak cache releases unreferenced sections beyond the LRU") {
    Fx              f;
    ProviderOptions po;
    po.strongCacheSize = 2;
    SourceProvider                      p(f.store, f.reg, f.clock, f.fileExec, CodecOptions{}, po);
    std::weak_ptr<const FullDataSource> w;
    {
        auto s = p.get(sec::encode(6, 0, 0));
        w      = s;
    }
    CHECK_FALSE(w.expired()); // held by the strong LRU
    (void)p.get(sec::encode(6, 1, 0));
    (void)p.get(sec::encode(6, 2, 0)); // evicts the first from the strong list
    CHECK(w.expired());
}

TEST_CASE("provider: getAdj equals clearAllNonAdjData of the full section") {
    Fx   f;
    auto pos = sec::encode(6, 1, 1);
    auto in  = f.make(6, 1, 1, f.stone, 10, 0, 120);
    REQUIRE(f.up().update(in));

    struct D {
        Direction    d;
        AdjDirection a;
    };
    for (D d : {
             D{Direction::North, AdjDirection::North},
             D{Direction::South, AdjDirection::South},
             D{Direction::West,  AdjDirection::West },
             D{Direction::East,  AdjDirection::East }
    }) {
        auto adj = f.provider->getAdj(pos, d.d);
        REQUIRE(adj);
        FullDataSource full = *f.provider->get(pos);
        full.clearAllNonAdjData(d.a);
        CHECK((*adj)->contentEquals(full));
    }
    CHECK_FALSE(f.provider->getAdj(pos, Direction::Up).has_value());

    auto missing = f.provider->getAdj(sec::encode(6, 9, 9), Direction::North);
    REQUIRE(missing);
    CHECK((*missing)->isEmpty);
}

TEST_CASE("provider: corrupt record is erased and reported as null") {
    Fx   f;
    auto pos = sec::encode(6, 0, 0);
    REQUIRE(f.up().update(f.make(6, 0, 0, f.stone, 10, 0, 4)));
    auto rec = f.store.get(pos);
    REQUIRE(rec);
    REQUIRE(rec->has_value());
    StoreRecord bad = **rec;
    bad.data.resize(3); // truncated
    REQUIRE(f.store.put(bad));
    f.provider->invalidate(pos);
    CHECK(f.provider->get(pos) == nullptr);
    CHECK(f.store.size() == 0);
}

TEST_CASE("provider: getAsync / updateAsync run on the executors") {
    Fx   f;
    auto pos = sec::encode(6, 0, 0);

    auto fut = f.provider->updateAsync(f.make(6, 0, 0, f.stone, 10, 0, 4));
    CHECK_FALSE(fut->isDone());
    CHECK(f.up().pendingAsyncCount() == 1);
    f.fileExec.runAll();
    REQUIRE(fut->isDone());
    CHECK(fut->result().value_or(false));
    CHECK(f.up().pendingAsyncCount() == 0);

    bool got = false;
    f.provider->getAsync(pos, [&](std::shared_ptr<const FullDataSource> s) { got = s && !s->isEmpty; });
    CHECK_FALSE(got);
    f.fileExec.runAll();
    CHECK(got);

    f.fileExec.shutdown();
    auto rejected = f.provider->updateAsync(f.make(6, 1, 0, f.stone, 10, 0, 4));
    REQUIRE(rejected->isDone());
    CHECK_FALSE(rejected->result().value_or(true));
    bool nullGot = false;
    f.provider->getAsync(pos, [&](std::shared_ptr<const FullDataSource> s) { nullGot = (s == nullptr); });
    CHECK(nullGot);
}

TEST_CASE("updater: listeners fire only on change, timestamps stamped") {
    Fx                         f;
    auto                       pos = sec::encode(6, 0, 0);
    std::vector<std::uint64_t> seen;
    auto                       id = f.provider->addUpdateListener([&](std::uint64_t p) { seen.push_back(p); });

    f.clock.setMillis(1000);
    auto in = f.make(6, 0, 0, f.stone, 10, 15, 16); // sky 15 == what the bake produces
    CHECK(f.up().update(in).value_or(false));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == pos);
    CHECK(f.provider->timestamp(pos) == std::optional<std::int64_t>{1000});

    f.clock.setMillis(2000);
    auto again = f.up().update(in); // identical input -> no change
    REQUIRE(again);
    CHECK_FALSE(*again);
    CHECK(seen.size() == 1);
    CHECK(f.provider->timestamp(pos) == std::optional<std::int64_t>{1000}); // not rewritten

    // empty-mapping input is a no-op
    auto empty = std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));
    CHECK_FALSE(f.up().update(empty).value_or(true));

    f.provider->removeUpdateListener(id);
    CHECK(f.up().update(f.make(6, 0, 0, f.dirt, 12, 0, 16)).value_or(false));
    CHECK(seen.size() == 1);

    CHECK_FALSE(f.provider->timestamp(sec::encode(6, 5, 5)).has_value());
}

TEST_CASE("updater: sky light is baked only at detail 6") {
    Fx f;
    // detail 6: stored top point gets maxSkyLight although the input had 0
    REQUIRE(f.up().update(f.make(6, 0, 0, f.stone, 10, /*sky*/ 0, 4)));
    auto leaf = f.provider->get(sec::encode(6, 0, 0));
    REQUIRE(leaf);
    CHECK(fdp::skyLight(leaf->column(0, 0)[0]) == 15);

    // detail 7: untouched
    REQUIRE(f.up().update(f.make(7, 0, 0, f.stone, 10, /*sky*/ 0, 4)));
    auto mid = f.provider->get(sec::encode(7, 0, 0));
    REQUIRE(mid);
    CHECK(fdp::skyLight(mid->column(0, 0)[0]) == 0);

    // no-sky levels bake 0
    Fx dark(0);
    REQUIRE(dark.up().update(dark.make(6, 0, 0, dark.stone, 10, 7, 4)));
    CHECK(fdp::skyLight(dark.provider->get(sec::encode(6, 0, 0))->column(0, 0)[0]) == 0);
}

TEST_CASE("updater: concurrent same-position updates serialise") {
    Fx            f;
    auto          pos        = sec::encode(6, 0, 0);
    constexpr int kThreads   = 4;
    constexpr int kPerThread = 8;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                auto s  = std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));
                int  id = s->mapping.addIfAbsent(f.reg.defaultBiome(), f.stone);
                // each (thread, iteration) owns one unique column
                const int col = t * kPerThread + i;
                s->setSingleColumn({fdp::encode(id, 5, 60, 0, 0)}, col, 0, 5, 0);
                s->isEmpty = false;
                (void)f.up().update(s);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto result = f.provider->get(pos);
    REQUIRE(result);
    for (int col = 0; col < kThreads * kPerThread; ++col) {
        CHECK_MESSAGE(result->column(col, 0).size() == 1, "column ", col, " lost");
    }
    CHECK(f.up().locks().liveKeyCount() == 0);
}

TEST_CASE("propagator: 4 leaves -> parent equals direct updateFromDataSource, flags cleared/set") {
    Fx                  f;
    UpdatePropagator    prop(*f.provider, f.propExec, 1);
    const std::uint64_t parentPos = sec::encode(7, 0, 0);

    std::vector<std::shared_ptr<FullDataSource>> leaves;
    const BlockInfo*                             blocks[4] = {f.stone, f.dirt, f.stone, f.dirt};
    for (int i = 0; i < 4; ++i) {
        leaves.push_back(f.make(6, i & 1, i >> 1, blocks[i], 10 + i * 3, 0));
        REQUIRE(f.up().update(leaves.back()));
    }
    CHECK(f.store.positionsToPropagateUp(0, 0, 100)->size() == 4);

    std::vector<std::uint64_t> fired;
    f.provider->addUpdateListener([&](std::uint64_t p) { fired.push_back(p); });

    const int rounds = settle(f, prop);
    CHECK(rounds < 200);

    // expected: apply the leaves directly in the propagator's order (nearest first: (0,0),(1,0),(0,1),(1,1))
    FullDataSource      expected = FullDataSource::createEmpty(parentPos);
    const std::uint64_t order[4] =
        {sec::encode(6, 0, 0), sec::encode(6, 1, 0), sec::encode(6, 0, 1), sec::encode(6, 1, 1)};
    for (std::uint64_t p : order) {
        auto stored = f.provider->get(p);
        REQUIRE(stored);
        REQUIRE(updateFromDataSource(expected, *stored, UpdateOptions{}).has_value());
    }
    auto parent = f.provider->get(parentPos);
    REQUIRE(parent);
    CHECK(parent->contentEquals(expected));
    CHECK_FALSE(parent->isEmpty);

    // flags: leaves cleared, parent chain flags consumed along the chain 7..13
    for (std::uint64_t p : order) {
        auto rec = f.store.get(p, StorePart::Meta);
        REQUIRE(rec);
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::ApplyToParent));
    }
    // The chain ends at 13: a single corner column covers only 1 of the 4 columns of a 2x2 group at 14 (gen EMPTY)
    for (int d = 7; d <= ROOT_SECTION_DETAIL_LEVEL - 1; ++d) {
        auto rec = f.store.get(sec::encode(static_cast<std::int8_t>(d), 0, 0), StorePart::Meta);
        REQUIRE(rec);
        REQUIRE(rec->has_value());
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::ApplyToParent)); // consumed by the next level up
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::ApplyToChildren));
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::Regenerate));
    }
    CHECK(
        f.store.get(sec::encode(static_cast<std::int8_t>(ROOT_SECTION_DETAIL_LEVEL), 0, 0)).value().has_value() == false
    );
    // listeners saw each ancestor exactly once (7..13)
    CHECK(fired.size() == 7);
    CHECK(f.propExec.queueSize() == 0);
    CHECK(prop.updatingCount() == 0);

    // The parent itself carried ApplyToParent right after its own update (true < ROOT): check mid-way
    // with a fresh setup limited to one level.
    Fx               g;
    UpdatePropagator prop2(*g.provider, g.propExec, 1);
    for (int i = 0; i < 4; ++i) REQUIRE(g.up().update(g.make(6, i & 1, i >> 1, blocks[i], 10, 0, 4)));
    prop2.tick(kOrigin);
    g.propExec.runAll();
    auto mid = g.store.get(parentPos, StorePart::Meta);
    REQUIRE(mid);
    REQUIRE(mid->has_value());
    CHECK((*mid)->meta.hasFlag(StoreFlag::ApplyToParent));
    CHECK_FALSE((*mid)->meta.hasFlag(StoreFlag::ApplyToChildren));
}

TEST_CASE("propagator: unchanged parent stops propagation") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);
    for (int i = 0; i < 4; ++i) REQUIRE(f.up().update(f.make(6, i & 1, i >> 1, f.stone, 10, 0, 4)));
    settle(f, prop);

    const auto parentPos = sec::encode(7, 0, 0);
    const auto grandPos  = sec::encode(8, 0, 0);
    f.clock.advanceMillis(5000);
    const auto parentTs = f.provider->timestamp(parentPos);
    const auto grandTs  = f.provider->timestamp(grandPos);

    int fired = 0;
    f.provider->addUpdateListener([&](std::uint64_t) { ++fired; });

    // Re-flag a leaf without changing its data: parent is re-merged, nothing changes.
    REQUIRE(f.store.setFlag(sec::encode(6, 1, 1), StoreFlag::ApplyToParent, true));
    f.provider->invalidate(sec::encode(6, 1, 1));
    settle(f, prop);

    CHECK(fired == 0);
    CHECK(f.provider->timestamp(parentPos) == parentTs);
    CHECK(f.provider->timestamp(grandPos) == grandTs);
    // flag was still cleared in the "finally"
    CHECK(f.store.positionsToPropagateUp(0, 0, 10)->empty());
}

TEST_CASE("propagator: task cap is 10 x threads and tasks are not duplicated") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);
    CHECK(prop.maxTaskCount() == 10);
    // 40 leaves in 40 distinct parents
    for (int i = 0; i < 40; ++i) REQUIRE(f.up().update(f.make(6, i * 2, 0, f.stone, 10, 0, 4)));

    auto s1 = prop.tick(kOrigin);
    // The store query is capped at `max` positions (Java getParentPositionsToUpdate(..., max))
    CHECK(s1.parentTasks == 10);
    CHECK(f.propExec.queueSize() == 10);
    CHECK(prop.updatingCount() == 10);

    // already-queued parents are skipped, the next nearest ones are not (list is capped at max)
    auto s2 = prop.tick(kOrigin);
    CHECK(s2.parentTasks == 0);
    CHECK(f.propExec.queueSize() == 10);

    // tasks hold the nearest parents: x = 0, 2, 4, ... in order
    f.propExec.runAll();
    CHECK(prop.updatingCount() == 0);
    for (int i = 0; i < 10; ++i) {
        auto rec = f.store.get(sec::encode(7, i, 0), StorePart::Meta);
        REQUIRE(rec);
        CHECK(rec->has_value());
    }
    CHECK_FALSE(f.store.get(sec::encode(7, 10, 0), StorePart::Meta).value().has_value());
}

TEST_CASE("propagator: locked parent defers (tryLock), flags kept for the next tick") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);
    for (int i = 0; i < 4; ++i) REQUIRE(f.up().update(f.make(6, i & 1, i >> 1, f.stone, 10, 0, 4)));

    {
        auto held = f.up().locks().lock(sec::encode(7, 0, 0));
        prop.tick(kOrigin);
        f.propExec.runAll();
        CHECK(prop.updatingCount() == 0);
        CHECK(f.store.positionsToPropagateUp(0, 0, 10)->size() == 4); // untouched
    }
    settle(f, prop);
    CHECK(f.store.positionsToPropagateUp(0, 0, 10)->empty());
    CHECK(f.provider->get(sec::encode(7, 0, 0))->isEmpty == false);
}

TEST_CASE("propagator: child (down) propagation downsamples and flags leaves for regeneration") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);
    const auto       parentPos = sec::encode(7, 0, 0);

    auto in             = f.make(7, 0, 0, f.stone, 20, 0);
    in->applyToParent   = false;
    in->applyToChildren = true;
    REQUIRE(f.up().update(in));
    CHECK(f.store.positionsToPropagateDown(0, 0, 10)->size() == 1);

    auto t = prop.tick(kOrigin);
    CHECK(t.childTasks == 1);
    f.propExec.runAll();

    auto parent = f.provider->get(parentPos);
    for (int i = 0; i < 4; ++i) {
        const auto childPos = sec::child(parentPos, i);
        auto       child    = f.provider->get(childPos);
        REQUIRE(child);
        FullDataSource expected = FullDataSource::createEmpty(childPos);
        REQUIRE(updateFromDataSource(expected, *parent, UpdateOptions{}).has_value());
        bakeDataSourceSkyLight(expected, 15); // the updater bakes sky light at detail 6
        CHECK_FALSE(child->isEmpty);
        CHECK(child->contentEquals(expected));
        auto rec = f.store.get(childPos, StorePart::Meta);
        REQUIRE(rec);
        REQUIRE(rec->has_value());
        CHECK((*rec)->meta.hasFlag(StoreFlag::Regenerate)); // leaf: generate
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::ApplyToChildren));
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::ApplyToParent));
    }
    CHECK(f.store.positionsToRegen(0, 0, -1, 10)->size() == 4);
    CHECK(f.store.positionsToPropagateDown(0, 0, 10)->empty()); // parent flag cleared
    CHECK(f.store.positionsToPropagateUp(0, 0, 10)->empty());
}

TEST_CASE("propagator: non-leaf children get ApplyToChildren, detail >= 12 is never downsampled") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);

    auto mid             = f.make(9, 0, 0, f.stone, 20, 0);
    mid->applyToParent   = false;
    mid->applyToChildren = true;
    REQUIRE(f.up().update(mid));
    prop.tick(kOrigin);
    f.propExec.runAll();
    for (int i = 0; i < 4; ++i) {
        auto rec = f.store.get(sec::child(sec::encode(9, 0, 0), i), StorePart::Meta);
        REQUIRE(rec);
        REQUIRE(rec->has_value());
        CHECK((*rec)->meta.hasFlag(StoreFlag::ApplyToChildren));
        CHECK_FALSE((*rec)->meta.hasFlag(StoreFlag::Regenerate));
    }

    // let it cascade to the leaves
    settle(f, prop);
    CHECK(f.store.positionsToPropagateDown(0, 0, 10)->empty());
    CHECK(f.store.positionsToRegen(0, 0, -1, 1000)->size() == 64); // 4^3 leaves

    Fx               g;
    UpdatePropagator prop2(*g.provider, g.propExec, 1);
    auto             top = g.make(HIGHEST_DOWNSAMPLE_DETAIL_LEVEL, 0, 0, g.stone, 20, 0, 8);
    top->applyToParent   = false;
    top->applyToChildren = true;
    REQUIRE(g.up().update(top));
    const auto before = g.store.size();
    prop2.tick(kOrigin);
    g.propExec.runAll();
    CHECK(g.store.size() == before);
    CHECK(g.store.positionsToPropagateDown(0, 0, 10)->empty());
}

TEST_CASE("propagator: deterministic order, nearest parent first") {
    Fx               f;
    UpdatePropagator prop(*f.provider, f.propExec, 1);
    REQUIRE(f.up().update(f.make(6, 40, 0, f.stone, 10, 0, 4)));
    REQUIRE(f.up().update(f.make(6, 0, 0, f.stone, 10, 0, 4)));
    REQUIRE(f.up().update(f.make(6, -10, 0, f.stone, 10, 0, 4)));
    std::vector<std::uint64_t> order;
    f.provider->addUpdateListener([&](std::uint64_t p) {
        if (sec::detail(p) == 7) order.push_back(p);
    });
    prop.tick(kOrigin);
    f.propExec.runAll();
    // Manhattan distance of the leaf min corners: x=0 -> 0, x=-10 -> 640, x=40 -> 2560
    REQUIRE(order.size() == 3);
    CHECK(order[0] == sec::encode(7, 0, 0));
    CHECK(order[1] == sec::encode(7, -5, 0));
    CHECK(order[2] == sec::encode(7, 20, 0));
}

TEST_CASE("chunk hash store / tracker") {
    MemoryChunkHashStore store;
    ChunkHashTracker     tracker(store);
    CHECK(store.get({1, 2}) == 0);
    tracker.notePending(77, {1, 2}, 0x1234);
    tracker.notePending(77, {0, 5}, -9);
    tracker.notePending(78, {3, 3}, 4);
    CHECK(store.get({1, 2}) == 0); // not before the section is saved
    CHECK(tracker.commit(77) == 2);
    CHECK(store.get({1, 2}) == 0x1234);
    CHECK(store.get({0, 5}) == -9);
    CHECK(store.get({3, 3}) == 0);
    tracker.discard(78);
    CHECK(tracker.pendingSectionCount() == 0);
    CHECK(tracker.commit(78) == 0);
}
