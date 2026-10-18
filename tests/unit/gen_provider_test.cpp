#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "dh/fulldata/FullDataSource.h"
#include "dh/gen/FakeChunkGenerator.h"
#include "dh/gen/GenQueue.h"
#include "dh/gen/GeneratedProvider.h"
#include "dh/gen/LodRequestModule.h"
#include "dh/gen/WorldGenerator.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/thread/ManualExecutor.h"
#include "golden_loader.h"

using namespace dh;

namespace {

/// IRetrievalQueue that only exists so the provider has "a queue" (genpos replay).
struct NullQueue final : IRetrievalQueue {
    std::shared_ptr<RetrievalFuture> submit(std::uint64_t, std::uint8_t) override {
        return std::make_shared<RetrievalFuture>();
    }
    void        startAndSetTargetPos(BlockPos2D) override {}
    void        removeRetrievalRequestIf(std::function<bool(std::uint64_t)>) override {}
    std::size_t waitingTaskCount() const override { return 0; }
    std::size_t inProgressTaskCount() const override { return 0; }
    void        shutdown(bool) override {}
};

/// Writes a record that has only a genSteps blob (Java stubbed repo: existsWithKey + getColumnGenerationStepForPos).
void putSteps(MemoryLodStore& store, std::uint64_t pos, const std::array<std::int8_t, 4096>* steps) {
    StoreRecord rec;
    rec.pos = pos;
    if (steps != nullptr) {
        FullDataSource src       = FullDataSource::createEmpty(pos);
        src.columnGenerationStep = *steps;
        auto enc                 = encode(src);
        REQUIRE(enc.has_value());
        rec.genSteps = enc->genSteps;
    }
    REQUIRE(store.put(rec, StorePart::GenSteps).has_value());
}

struct ProviderFx {
    MapBlockRegistry  reg{false};
    ManualClock       clock;
    ManualExecutor    fileExec;
    MemoryLodStore    store;
    GeneratedProvider provider;

    explicit ProviderFx(GeneratorPlan plan = GeneratorPlan::SurfaceThenChunks)
    : provider(store, reg, clock, fileExec, CodecOptions{}, ProviderOptions{}, GeneratedProviderOptions{plan, 2}) {}
};

} // namespace

TEST_CASE("genpos golden replay") {
    DH_GOLDEN_SUITE_OR_SKIP(suite, "genpos");
    CHECK(suite.cases.size() >= 300);

    NullQueue queue;
    int       passed = 0;
    int       failed = 0;
    for (const auto& c : suite.cases) {
        const auto plan     = static_cast<GeneratorPlan>(c.in["plan"].get<int>());
        const auto pos      = test::hexU64(c.in["pos"]);
        const auto detail   = static_cast<std::uint8_t>(c.in["generatorDetailLevel"].get<int>());
        const bool hasQueue = c.in["hasQueue"].get<bool>();

        ProviderFx fx(plan);
        if (hasQueue) fx.provider.setRetrievalQueue(&queue);
        for (const auto& r : c.in["repo"]) {
            std::array<std::int8_t, 4096> steps{};
            std::size_t                   idx = 0;
            for (const auto& run : r["steps"]) {
                const auto v = static_cast<std::int8_t>(run[0].get<int>());
                const int  n = run[1].get<int>();
                for (int i = 0; i < n && idx < steps.size(); ++i) steps[idx++] = v;
            }
            const bool hasData = !r["steps"].empty();
            putSteps(fx.store, test::hexU64(r["pos"]), hasData ? &steps : nullptr);
        }

        const auto got = fx.provider.positionsToRetrieve(pos, detail);
        bool       ok;
        if (c.out["positions"].is_null()) {
            ok = !got.has_value();
        } else {
            const auto want = test::hexU64Array(c.out["positions"]);
            ok              = got.has_value() && *got == want;
        }
        if (ok) ++passed;
        else {
            ++failed;
            MESSAGE("genpos mismatch: " << c.name);
        }
    }
    MESSAGE("genpos replay: " << passed << " pass / " << failed << " fail");
    CHECK(failed == 0);
}

TEST_CASE("genpos rules: required step, <= vs <") {
    using P = GeneratedProvider;
    CHECK(P::requiredGenerationStep(GeneratorPlan::SurfaceOnly, 6) == 5);
    CHECK(P::requiredGenerationStep(GeneratorPlan::SurfaceOnly, 20) == 5);
    CHECK(P::requiredGenerationStep(GeneratorPlan::SurfaceThenChunks, 7) == 5);
    CHECK(P::requiredGenerationStep(GeneratorPlan::SurfaceThenChunks, 6) == 8);
    CHECK(P::requiredGenerationStep(GeneratorPlan::ChunksOnly, 12) == 8);

    std::array<std::int8_t, 4> allFeatures{8, 8, 8, 8};
    std::array<std::int8_t, 4> allLight{9, 9, 9, 9};
    // :403 uses <=, a column AT the required step is not "fully generated"
    CHECK_FALSE(P::isFullyGenerated(allFeatures, 8));
    CHECK(P::isFullyGenerated(allLight, 8));
    CHECK(P::isFullyGenerated(allFeatures, 5));
    // :465 uses <, a child whose minimum EQUALS the required step needs no generation
    CHECK_FALSE(P::childNeedsGeneration(allFeatures, 8));
    CHECK(P::childNeedsGeneration(std::array<std::int8_t, 4>{9, 7, 9, 9}, 8));
    CHECK(P::childNeedsGeneration(std::array<std::int8_t, 4>{9, 0, 9, 9}, 5));
    CHECK(P::childNeedsGeneration(std::array<std::int8_t, 4>{9, -1, 9, 9}, 5));
    CHECK_FALSE(P::generationStepsAreFullyGenerated(std::array<std::int8_t, 3>{9, 0, 9}));
    CHECK(P::generationStepsAreFullyGenerated(std::array<std::int8_t, 3>{1, 2, 9}));
}

// ----------------------------------------------------------------------------------------------------
// end-to-end

namespace {

struct E2E {
    MapBlockRegistry reg{false};
    ManualClock      clock;
    ManualExecutor   fileExec, chunkExec, queueExec, genPool;
    MemoryLodStore   store;
    const BlockInfo* stone = nullptr;
    // destroyed last: queue futures hold callbacks into the provider
    std::unique_ptr<GeneratedProvider>  provider;
    std::unique_ptr<FakeChunkGenerator> chunkGen;
    std::unique_ptr<WorldGenerator>     worldGen;
    std::unique_ptr<GenQueue>           queue;

    E2E() {
        BlockInfo s;
        s.serial  = "minecraft:stone";
        s.opacity = 16;
        stone     = reg.addBlock(std::move(s));
        chunkGen  = std::make_unique<FakeChunkGenerator>(chunkExec, reg.air(), stone, reg.defaultBiome());
        DhWorldGeneratorOptions wo;
        wo.plan  = GeneratorPlan::ChunksOnly;
        worldGen = std::make_unique<WorldGenerator>(*chunkGen, wo);
        queue    = std::make_unique<GenQueue>(*worldGen, queueExec, genPool, 1, clock);
        provider = std::make_unique<GeneratedProvider>(
            store,
            reg,
            clock,
            fileExec,
            CodecOptions{},
            ProviderOptions{},
            GeneratedProviderOptions{wo.plan, 2}
        );
        REQUIRE(provider->setRetrievalQueue(queue.get()));
    }

    void pump() {
        for (int i = 0; i < 10000; ++i) {
            const std::size_t n = queueExec.runAll() + genPool.runAll() + chunkExec.runAll() + fileExec.runAll();
            if (n == 0) break;
        }
    }
};

} // namespace

TEST_CASE("e2e: missing section is queued, generated, lit, built and stored") {
    E2E        fx;
    const auto pos = sec::encode(6, 0, 0);

    auto missing = fx.provider->positionsToRetrieve(pos, 6);
    REQUIRE(missing.has_value());
    CHECK(*missing == std::vector<std::uint64_t>{pos});

    struct L final : IWorldGenCompleteListener {
        std::vector<std::uint64_t> done;
        bool                       shouldDoWorldGen() override { return true; }
        std::optional<BlockPos2D>  targetPosForGeneration() override { return BlockPos2D{0, 0}; }
        void                       onWorldGenTaskComplete(std::uint64_t p) override { done.push_back(p); }
    } listener;
    fx.provider->addWorldGenCompleteListener(&listener);

    auto fut = fx.provider->queueRetrieval(pos);
    REQUIRE(fut != nullptr);
    CHECK(fx.provider->queueRetrieval(pos) == fut); // de-dup
    fx.queue->startAndSetTargetPos(BlockPos2D{32, 32});
    fx.pump();

    REQUIRE(fut->isDone());
    CHECK(fut->get()->kind == RetrievalResult::Kind::Success);
    CHECK(listener.done == std::vector<std::uint64_t>{pos});
    CHECK(fx.provider->queuedRetrievalCount() == 0);
    REQUIRE(fx.chunkGen->calls.size() == 1);
    CHECK(fx.chunkGen->calls[0].widthChunks == 4);
    CHECK(fx.chunkGen->calls[0].step == GenerationStep::Features);
    CHECK(fx.chunkGen->chunksCreated() == 16);

    auto section = fx.provider->get(pos);
    REQUIRE(section != nullptr);
    CHECK_FALSE(section->isEmpty);
    for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
        CHECK(
            section->columnGenerationStep[static_cast<std::size_t>(i)]
            == static_cast<std::int8_t>(GenerationStep::Light)
        );
        CHECK_FALSE(section->columns[static_cast<std::size_t>(i)].empty());
    }

    auto after = fx.provider->positionsToRetrieve(pos, 6);
    REQUIRE(after.has_value());
    CHECK(after->empty()); // fully generated now
}

TEST_CASE("e2e: Split result subdivides through LodRequestSet") {
    LodRequestSet set; // outlives fx: queue futures call back into it on shutdown
    E2E           fx;
    const auto    parent = sec::encode(7, 0, 0); // chunk-only generator cannot build detail 7

    set.tryQueuePosForRetrieval(*fx.provider, parent);
    CHECK(set.missingCount() == 1);
    CHECK(set.isMissing(parent));

    fx.queue->startAndSetTargetPos(BlockPos2D{0, 0});
    CHECK(set.startQueued(*fx.provider, BlockPos2D{0, 0}) == 1);
    CHECK(set.isQueued(parent));
    fx.pump();

    CHECK_FALSE(set.isQueued(parent));
    // the 4 children are now missing, in ascending pos order (distance ties)
    CHECK(set.missingCount() == 4);
    std::vector<std::uint64_t> kids;
    sec::forEachChild(parent, [&](std::uint64_t c) { kids.push_back(c); });
    for (auto k : kids) CHECK(set.isMissing(k));
    CHECK_FALSE(fx.provider->canRegenerate());

    // and they can be generated
    CHECK(set.startQueued(*fx.provider, BlockPos2D{0, 0}) == 4);
    fx.queue->startAndSetTargetPos(BlockPos2D{0, 0}); // what LodRequestModule::tick does every tick
    fx.pump();
    CHECK(set.missingCount() == 0);
    CHECK(set.queuedCount() == 0);
    for (auto k : kids) {
        auto left = fx.provider->positionsToRetrieve(k, 6);
        REQUIRE(left.has_value());
        CHECK(left->empty());
    }
}

TEST_CASE("e2e: close cancels outstanding requests exactly once") {
    E2E        fx;
    const auto pos = sec::encode(6, 0, 0);
    auto       fut = fx.provider->queueRetrieval(pos);
    REQUIRE(fut != nullptr);
    fx.queue->startAndSetTargetPos(BlockPos2D{0, 0});
    fx.queueExec.runAll();
    fx.genPool.runAll(); // generate() called, chunk batch still pending on chunkExec
    CHECK(fx.worldGen->inFlight() == 1);
    CHECK(fx.chunkGen->pendingBatches() == 1);

    fx.queue->shutdown(true);
    CHECK(fx.worldGen->inFlight() == 0);
    CHECK(fx.chunkGen->cancelCount >= 1);
    REQUIRE(fut->isDone());
    CHECK(fut->get()->kind == RetrievalResult::Kind::Rejected);

    fx.pump(); // stale tasks must not resurrect anything
    CHECK(fx.provider->get(pos)->isEmpty);
    CHECK(fx.provider->queuedRetrievalCount() == 0);
}

TEST_CASE("WorldGenerator: close fires Cancelled once, late callbacks ignored") {
    MapBlockRegistry reg{false};
    ManualExecutor   chunkExec, pool;
    BlockInfo        s;
    s.serial                      = "minecraft:stone";
    auto*                   stone = reg.addBlock(std::move(s));
    FakeChunkGenerator      chunkGen(chunkExec, reg.air(), stone, reg.defaultBiome());
    DhWorldGeneratorOptions wo;
    wo.plan = GeneratorPlan::ChunksOnly;
    WorldGenerator gen(chunkGen, wo);

    int             calls = 0;
    ErrorCode       code  = ErrorCode::Unknown;
    GenerateRequest req;
    req.sectionPos  = sec::encode(6, 0, 0);
    req.widthChunks = 4;
    gen.generate(req, pool, nullptr, [&](Result<std::shared_ptr<FullDataSource>> r) {
        ++calls;
        if (!r) code = r.error().code;
    });
    gen.close();
    CHECK(calls == 1);
    CHECK(code == ErrorCode::Cancelled);
    chunkExec.runAll();
    pool.runAll();
    CHECK(calls == 1);

    gen.generate(req, pool, nullptr, [&](Result<std::shared_ptr<FullDataSource>>) { ++calls; });
    CHECK(calls == 2); // closed generator answers immediately with Cancelled
}

TEST_CASE("WorldGenerator: detail mapping and rough hook") {
    MapBlockRegistry   reg{false};
    ManualExecutor     chunkExec, pool;
    FakeChunkGenerator chunkGen(chunkExec, reg.air(), reg.air(), reg.defaultBiome());
    WorldGenerator     gen(chunkGen);
    CHECK(gen.smallestDataDetailLevel() == 0);
    CHECK(gen.largestDataDetailLevel() == 12);
    gen.setGeneratorPlan(GeneratorPlan::ChunksOnly);
    CHECK(gen.largestDataDetailLevel() == 0);
    CHECK(WorldGenerator::targetStepFor(GenerationMode::InternalServer) == GenerationStep::Light);
    CHECK(WorldGenerator::targetStepFor(GenerationMode::PreExistingOnly) == GenerationStep::Empty);
    CHECK_FALSE(WorldGenerator::chunkGenAllowed(GeneratorPlan::SurfaceOnly, 0, GenerationMode::Features));
    CHECK_FALSE(WorldGenerator::chunkGenAllowed(GeneratorPlan::SurfaceThenChunks, 0, GenerationMode::PreExistingOnly));
    CHECK(WorldGenerator::chunkGenAllowed(GeneratorPlan::ChunksOnly, 0, GenerationMode::PreExistingOnly));
    CHECK_FALSE(WorldGenerator::chunkGenAllowed(GeneratorPlan::ChunksOnly, 1, GenerationMode::Features));

    GenerateRequest req;
    req.sectionPos = sec::encode(7, 0, 0);
    req.dataDetail = 1;
    ErrorCode code = ErrorCode::Unknown;
    gen.generate(req, pool, nullptr, [&](Result<std::shared_ptr<FullDataSource>> r) {
        code = r ? ErrorCode::Unknown : r.error().code;
    });
    CHECK(code == ErrorCode::Unsupported);

    bool roughCalled = false;
    gen.setRoughGenerator([&](const GenerateRequest& r, IExecutor&, IWorldGenerator::DoneCallback done) {
        roughCalled = true;
        done(std::make_shared<FullDataSource>(FullDataSource::createEmpty(r.sectionPos)));
    });
    bool ok = false;
    gen.generate(req, pool, nullptr, [&](Result<std::shared_ptr<FullDataSource>> r) { ok = r.has_value(); });
    CHECK(roughCalled);
    CHECK(ok);
}

TEST_CASE("LodRequestModule: starts and stops the queue with the listener") {
    ProviderFx fx;
    struct L   final : IWorldGenCompleteListener {
        bool                      want      = false;
        int                       completed = 0;
        bool                      shouldDoWorldGen() override { return want; }
        std::optional<BlockPos2D> targetPosForGeneration() override { return BlockPos2D{1, 2}; }
        void                      onWorldGenTaskComplete(std::uint64_t) override { ++completed; }
    } listener;

    int              created = 0;
    LodRequestModule mod(fx.provider, listener, [&] {
        ++created;
        return std::make_unique<NullQueue>();
    });

    mod.tick();
    CHECK_FALSE(mod.isWorldGenRunning());
    CHECK_FALSE(fx.provider.hasRetrievalQueue());

    listener.want = true;
    mod.tick();
    CHECK(mod.isWorldGenRunning());
    CHECK(fx.provider.hasRetrievalQueue());
    CHECK(created == 1);
    mod.tick();
    CHECK(created == 1);

    listener.want = false;
    mod.tick();
    CHECK_FALSE(mod.isWorldGenRunning());
    CHECK_FALSE(fx.provider.hasRetrievalQueue());

    listener.want = true;
    mod.setReadOnlyPredicate([] { return true; });
    mod.tick();
    CHECK_FALSE(mod.isWorldGenRunning());
}
