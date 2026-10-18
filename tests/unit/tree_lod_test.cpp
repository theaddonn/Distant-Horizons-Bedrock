// WP-G2: LodQuadTree tick / LodRenderSection lifecycle / RenderList property tests.
//
// Driven entirely by test doubles: ManualExecutor (render loader), RecordingVertexSink, a FakeDataSource
// (synthetic FullDataSources, no store/codec) and a FakeRetrievalQueue. Invariants checked:
//   * tiling: after settling with all data available the enabled set covers every in-bounds detail-6 cell
//     exactly once (no hole, no overlap), nothing enabled is out of bounds, a recursing root is never enabled
//   * detail levels: every enabled section sits at expected or expected-1 (LodQuadTree.java:627-628)
//   * parent persistence: a parent with < 4 renderable children stays enabled, its children disabled (:684-700)
//   * teleport: no stale enabled sections, removed roots are disposed (sink.remove)
//   * out of range: sections that fall out of bounds are disabled and marked renderDataDirty
//   * reload on provider update (queueReload / attachToProvider) re-uploads pos + 4 neighbours
//   * cancellation: a load cancelled before it runs never uploads
//   * render list: near-to-far order, culled sections absent, never-cull flag
//   * random seeds (16, or 200 with DH_GOLDEN_SLOW) over radius / player position / quality for the tiling +
//     teleport invariants
//   * empty data: ONE empty upload, canRender() true, requested for retrieval (Java semantics)
//   * a disposed section's late upload callback never removes the replacement section's buffers (removeIf)
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/glm.hpp>

#include "dh/config/Options.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/gen/RetrievalQueue.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/IVertexSink.h"
#include "dh/platform/testing/TestClientLevel.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/Frustum.h"
#include "dh/render/RenderList.h"
#include "dh/render/RenderUtil.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/storage/SourceProvider.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/tree/LodQuadTree.h"
#include "dh/tree/LodRenderSection.h"

using namespace dh;
using namespace dh::tree;
using sec::Pos;

namespace {

//==============//
// test doubles //
//==============//

/// Synthetic full data: every "available" position gets a few stone columns, everything else is empty.
struct FakeDataSource final : ISectionDataSource {
    MapBlockRegistry reg;
    const BlockInfo* stone = nullptr;
    /// positions (any detail) for which data exists; empty optional = everything
    std::optional<std::set<Pos>> available;
    int                          getCalls = 0, adjCalls = 0;
    /// built sections are immutable snapshots (like SourceProvider's), so they can be cached
    std::map<Pos, std::shared_ptr<const FullDataSource>> cache;
    /// speed knob for the seed loop: neighbours are empty strips (the centre data still drives the lifecycle)
    bool emptyAdjacent = false;

    FakeDataSource() {
        BlockInfo s;
        s.serial  = "minecraft:stone";
        s.opacity = 16;
        stone     = reg.addBlock(std::move(s));
    }

    bool has(Pos pos) const { return !available.has_value() || available->contains(pos); }

    std::shared_ptr<FullDataSource> make(Pos pos) {
        auto s = std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));
        if (has(pos)) {
            const int id = s->mapping.addIfAbsent(reg.defaultBiome(), stone);
            for (int x = 0; x < 2; x++) {
                for (int z = 0; z < 2; z++) {
                    s->setSingleColumn({fdp::encode(id, 20 + x, 60, 0, 15)}, x * 32, z * 32, 5, 0);
                }
            }
            s->isEmpty = false;
        }
        return s;
    }

    std::shared_ptr<const FullDataSource> cached(Pos pos) {
        auto it = cache.find(pos);
        if (it != cache.end() && it->second->isEmpty == !has(pos)) return it->second;
        std::shared_ptr<const FullDataSource> s = make(pos);
        cache[pos]                              = s;
        return s;
    }
    std::shared_ptr<const FullDataSource> get(Pos pos) override {
        getCalls++;
        return cached(pos);
    }
    std::shared_ptr<const FullDataSource> getAdj(Pos pos, Direction) override {
        adjCalls++;
        if (emptyAdjacent) {
            if (!emptyStrip) emptyStrip = std::make_shared<const FullDataSource>(FullDataSource::createEmpty(pos));
            return emptyStrip;
        }
        return cached(pos);
    }
    std::shared_ptr<const FullDataSource> emptyStrip;
};

struct FakeRetrievalQueue final : IRetrievalQueue {
    std::vector<std::pair<Pos, std::uint8_t>>       submitted;
    std::map<Pos, std::shared_ptr<RetrievalFuture>> futures;
    std::vector<std::function<bool(std::uint64_t)>> removeIfCalls;
    BlockPos2D                                      target{};

    std::shared_ptr<RetrievalFuture> submit(std::uint64_t sectionPos, std::uint8_t requiredGenDetail) override {
        submitted.emplace_back(sectionPos, requiredGenDetail);
        auto it = futures.find(sectionPos);
        if (it != futures.end()) return it->second;
        auto f              = std::make_shared<RetrievalFuture>();
        futures[sectionPos] = f;
        return f;
    }
    void startAndSetTargetPos(BlockPos2D t) override { target = t; }
    void removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) override {
        removeIfCalls.push_back(std::move(removeIf));
    }
    std::size_t waitingTaskCount() const override { return futures.size(); }
    std::size_t inProgressTaskCount() const override { return 0; }
    void        shutdown(bool) override {}
};

//=========//
// fixture //
//=========//

struct Fx {
    Options                      options = defaultOptions(4);
    FakeDataSource               data;
    TestClientLevel              level;
    RecordingVertexSink          sink;
    ManualExecutor               renderLoader;
    RenderSectionContext         ctx;
    std::unique_ptr<LodQuadTree> tree;

    explicit Fx(int radiusChunks = 32, BlockPos2D start = {0, 0}, LodQuadTreeDeps deps = {}) {
        options.client.graphics.quality.lodChunkRenderDistanceRadius = radiusChunks;
        ctx.data                                                     = &data;
        ctx.level                                                    = &level;
        ctx.registry                                                 = &data.reg;
        ctx.sink                                                     = &sink;
        ctx.renderLoader                                             = &renderLoader;
        ctx.mesh.quad.transparency                                   = Transparency::Complete;
        tree                                                         = std::make_unique<LodQuadTree>(
            options,
            LodQuadTree::viewDiameterInBlocks(options),
            start,
            ctx,
            std::move(deps)
        );
    }

    std::set<Pos> enabledSet() const {
        std::vector<LodQuadTree::Section> v;
        tree->collectEnabled(v);
        std::set<Pos> out;
        for (auto& s : v) out.insert(s->pos);
        return out;
    }

    /// One tick + drain the loader + answer every upload.
    void round(BlockPos2D player) {
        tree->tryTick(player);
        renderLoader.runAll();
        sink.completeAll();
    }

    /// Ticks until the enabled set is stable for two rounds and no upload happened. Returns rounds run.
    int settle(BlockPos2D player, int maxRounds = 60) {
        std::set<Pos> prev   = enabledSet();
        int           stable = 0;
        int           r      = 0;
        for (; r < maxRounds; r++) {
            const std::size_t uploadsBefore = sink.uploads().size();
            round(player);
            const std::set<Pos> now = enabledSet();
            if (now == prev && sink.uploads().size() == uploadsBefore) {
                if (++stable >= 2) break;
            } else {
                stable = 0;
            }
            prev = now;
        }
        return r;
    }

    std::int8_t expectedSectionDetail(BlockPos2D player, Pos pos) const {
        const DetailLevelMath m = tree->detailLevelMath();
        std::int8_t           e = m.calcExpectedDetailLevel(player, pos, tree->cameraZoom());
        e                       = math::min(e, m.minRootRenderDetailLevel);
        return static_cast<std::int8_t>(e + sec::SECTION_MINIMUM_DETAIL_LEVEL);
    }
};

/// Every in-bounds detail-6 cell of the tree is covered by exactly one enabled section; nothing enabled is
/// out of bounds, at root level, or outside [expected-1, expected].
void checkTiling(const Fx& f, BlockPos2D player, const char* what) {
    INFO(what);
    const std::set<Pos> enabled = f.enabledSet();
    REQUIRE_FALSE(enabled.empty());

    std::vector<LodQuadTree::Section> sections;
    f.tree->collectEnabled(sections);
    for (auto& s : sections) {
        INFO("enabled section ", sec::toString(s->pos));
        CHECK(s->canRender());
        CHECK(s->getRenderingEnabled());
        CHECK(f.tree->isSectionPosInBounds(s->pos));
        // Note: Java's "never render the root" rule (:676) only applies when the root recurses; a root AT
        // the expected level (coarse resolution settings) is enabled by onDesiredDetailLevel like any node.
        const std::int8_t expected = f.expectedSectionDetail(player, s->pos);
        const std::int8_t d        = sec::detail(s->pos);
        CHECK((d == expected || d == expected - 1));
    }

    // coverage: walk every in-bounds leaf (detail 6) cell inside the tree square
    const std::int32_t half  = f.tree->diameterInBlocks() / 2;
    const BlockPos2D   c     = f.tree->getCenterBlockPos();
    const Pos          minP  = sec::encodeContainingBlock(6, c.x - half, c.z - half);
    const Pos          maxP  = sec::encodeContainingBlock(6, c.x + half - 1, c.z + half - 1);
    int                cells = 0, holes = 0, overlaps = 0;
    for (std::int32_t x = sec::x(minP); x <= sec::x(maxP); x++) {
        for (std::int32_t z = sec::z(minP); z <= sec::z(maxP); z++) {
            const Pos cell = sec::encode(6, x, z);
            if (!f.tree->isSectionPosInBounds(cell)) continue;
            cells++;
            int covering = 0;
            for (Pos p = cell;; p = sec::parent(p)) {
                if (enabled.contains(p)) covering++;
                if (sec::detail(p) >= f.tree->treeRootDetailLevel) break;
            }
            if (covering == 0) holes++;
            if (covering > 1) overlaps++;
        }
    }
    CHECK(cells > 0);
    CHECK_EQ(holes, 0);
    CHECK_EQ(overlaps, 0);
}

} // namespace

//===========//
// unit bits //
//===========//

TEST_CASE("LodQuadTree: construction from Options") {
    Fx f(32);
    CHECK_EQ(f.tree->blockRenderDistanceDiameter, 32 * 16 * 2);
    CHECK_EQ(f.tree->treeRootDetailLevel, 10);
    CHECK_EQ(f.tree->treeLeafDetailLevel, 6);
    CHECK_EQ(LodQuadTree::rootDetailLevelFor(1024), 10);
    CHECK_EQ(LodQuadTree::rootDetailLevelFor(13273), 13);

    const DetailLevelMath m = f.tree->detailLevelMath();
    CHECK_EQ(m.detailDropOffDistanceUnit, 12 * 16); // MEDIUM: 12 chunks
    CHECK_EQ(m.maxLeafRenderDetailLevel, 0);        // BLOCK
    // calc(1024) = (int)(ln(1024/192)/ln 2) = 2; -1 -> 1; min(1, 10); min(1, 8); max(1, 0)
    CHECK_EQ(m.minRootRenderDetailLevel, 1);
    CHECK_EQ(m.calcDetailLevelFromDistance(0.0), 0);   // -inf -> INT_MIN -> clamp
    CHECK_EQ(m.calcDetailLevelFromDistance(1e30), 14); // ROOT_SECTION_DETAIL_LEVEL
}

TEST_CASE("LodQuadTree: isPosInWorldGenRange") {
    CHECK(LodQuadTree::isPosInWorldGenRange(sec::encode(6, 100, 100), 0, 0, 0));
    CHECK(LodQuadTree::isPosInWorldGenRange(sec::encode(6, 0, 0), 0, 0, 4));
    CHECK_FALSE(LodQuadTree::isPosInWorldGenRange(sec::encode(6, 100, 100), 0, 0, 4));
}

TEST_CASE("RenderUtil: near / far clip planes") {
    renderutil::NearClipInputs in;
    in.vanillaChunkRenderDistance     = 12;
    in.viewportWidth                  = 1920;
    in.viewportHeight                 = 1080;
    in.reduceOverdrawWithFastMovement = false;
    CHECK_EQ(renderutil::overdrawPrevention(in), 0.9f);
    in.vanillaChunkRenderDistance = 2;
    CHECK_EQ(renderutil::overdrawPrevention(in), 0.2f);
    in.overdrawPreventionSetting = 0.0f;
    CHECK_EQ(renderutil::overdrawPrevention(in), 0.05f);
    in.overdrawPreventionSetting      = 0.5f;
    in.reduceOverdrawWithFastMovement = true;
    in.avgCameraSpeedBlocksPerSec     = 100.0;
    CHECK_EQ(renderutil::overdrawPrevention(in), 0.5f * 0.2f); // speed range floored at 0.2

    // 12 chunks * 16 * 0.9 = 172.8 blocks, FOV 70 / 16:9 correction
    in                                = {};
    in.vanillaChunkRenderDistance     = 12;
    in.reduceOverdrawWithFastMovement = false;
    in.viewportWidth                  = 1920;
    in.viewportHeight                 = 1080;
    const float  near                 = renderutil::nearClipPlaneInBlocks(in);
    const double expected =
        172.8
        / std::sqrt(
            1.0 + std::pow(std::tan(70.0 / 180.0 * 3.141592653589793 / 2.0), 2) * (std::pow(1920.0 / 1080.0, 2) + 1.0)
        );
    CHECK(std::fabs(near - static_cast<float>(expected)) < 1e-3f);
    CHECK_EQ(renderutil::dhNearClipDistance(in), 7.5f);

    // > 1000 blocks above the world: the override wins (and the 7.5 cap does not apply)
    in.playerBlockY   = 320 + 1000 + 500;
    in.levelMaxHeight = 320;
    CHECK_EQ(renderutil::heightBasedNearClipOverrideBlockDistance(true, in.playerBlockY, 320), 500.0f);
    CHECK(renderutil::dhNearClipDistance(in) > 7.5f);

    CHECK_EQ(renderutil::farClipPlaneDistanceInBlocks(256), (256 * 16 + 512) * 2);
    CHECK_EQ(renderutil::farClipPlaneDistanceInBlocks(32, true), static_cast<float>((32 * 16 + 512) * std::sqrt(2.0)));
}

TEST_CASE("CameraZoom: fromProjection") {
    const glm::mat4 proj = glm::perspective(glm::radians(10.0f), 16.0f / 9.0f, 0.1f, 1000.0f); // 7x zoom vs 70
    CameraZoom      z = CameraZoom::fromProjection(proj, glm::vec3(0, 0, -1), 70.0, HorizontalQuality::Medium, 4, true);
    CHECK(z.isZoomed());
    CHECK(z.magnification <= 16.0); // capped at base^maxZoomQualityIncrease = 2^4
    CHECK(z.lookDirectionZ == doctest::Approx(-1.0));
    CHECK_FALSE(
        CameraZoom::fromProjection(proj, glm::vec3(0, 1, 0.01f), 70.0, HorizontalQuality::Medium, 4, true).isZoomed()
    );
    CHECK_FALSE(
        CameraZoom::fromProjection(proj, glm::vec3(0, 0, -1), 70.0, HorizontalQuality::Medium, 4, false).isZoomed()
    );
    const glm::mat4 normal = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    CHECK_FALSE(
        CameraZoom::fromProjection(normal, glm::vec3(0, 0, -1), 70.0, HorizontalQuality::Medium, 4, true).isZoomed()
    );
}

//================//
// tick invariants //
//================//

TEST_CASE("LodQuadTree: tiling with all data available, loads coarse-first") {
    Fx               f(32);
    const BlockPos2D player(100, -50);

    f.tree->tryTick(player);
    // every visited node is queued for loading; coarse (large detail) sections first
    REQUIRE(f.renderLoader.queueSize() > 0);
    f.renderLoader.runAll();
    REQUIRE(f.sink.uploads().size() > 1);
    std::int8_t last = 127;
    for (auto& u : f.sink.uploads()) {
        CHECK(sec::detail(u.sectionPos) <= last);
        last = sec::detail(u.sectionPos);
    }
    // nothing renderable before the sink answers
    CHECK(f.enabledSet().empty());
    f.sink.completeAll();

    const int rounds = f.settle(player);
    CHECK(rounds < 20);
    checkTiling(f, player, "settled");

    // a settled tree queues no more work
    f.tree->tryTick(player);
    CHECK_EQ(f.renderLoader.queueSize(), 0);
}

TEST_CASE("LodQuadTree: a parent stays enabled until all four children are renderable") {
    Fx               f(32);
    const BlockPos2D player(0, 0);

    f.tree->tryTick(player);
    f.renderLoader.runAll();
    // answer only the detail >= 7 uploads plus 3 of the 4 children of one detail-7 parent next to the player
    const Pos parentPos = sec::encode(7, 0, 0);
    const Pos heldChild = sec::child(parentPos, 3);
    for (auto& u : f.sink.uploads()) {
        if (!u.onDone) continue;
        const bool isChildOfParent = sec::detail(u.sectionPos) == 6 && sec::contains(parentPos, u.sectionPos);
        if (sec::detail(u.sectionPos) >= 7 || (isChildOfParent && u.sectionPos != heldChild)) {
            auto cb  = std::move(u.onDone);
            u.onDone = nullptr;
            cb(true);
        }
    }

    f.tree->tryTick(player);
    std::set<Pos> enabled = f.enabledSet();
    CHECK(enabled.contains(parentPos));
    for (int i = 0; i < 4; i++) CHECK_FALSE(enabled.contains(sec::child(parentPos, i)));
    // the parent's disabled children keep their flags consistent
    for (int i = 0; i < 4; i++) {
        auto v = f.tree->tryGetValue(sec::child(parentPos, i));
        REQUIRE(v.has_value());
        CHECK_FALSE((*v)->getRenderingEnabled());
    }
    // a neighbouring parent whose 4 children never answered stays enabled too (parent rendered instead)
    // once the held child answers, the parent hands over to its children
    for (auto& u : f.sink.uploads()) {
        if (u.onDone && u.sectionPos == heldChild) {
            auto cb  = std::move(u.onDone);
            u.onDone = nullptr;
            cb(true);
        }
    }
    f.tree->tryTick(player);
    enabled = f.enabledSet();
    CHECK_FALSE(enabled.contains(parentPos));
    for (int i = 0; i < 4; i++) CHECK(enabled.contains(sec::child(parentPos, i)));
    CHECK_FALSE(f.tree->tryGetValue(parentPos).value()->getRenderingEnabled());

    f.sink.completeAll();
    f.settle(player);
    checkTiling(f, player, "after the held child arrived");
}

TEST_CASE("LodQuadTree: teleport leaves no stale enabled nodes and disposes removed roots") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.settle(a);
    checkTiling(f, a, "at A");
    const std::set<Pos> atA = f.enabledSet();

    const BlockPos2D b(50'000, -70'000);
    f.settle(b);
    checkTiling(f, b, "at B");
    const std::set<Pos> atB = f.enabledSet();
    for (Pos p : atB) CHECK_FALSE(atA.contains(p));

    // every section that rendered at A was disposed: buffers removed, not present in the tree any more
    std::set<Pos> removed(f.sink.removes().begin(), f.sink.removes().end());
    for (Pos p : atA) {
        CHECK(removed.contains(p));
        CHECK_FALSE(f.tree->tryGetValue(p).has_value());
    }
}

TEST_CASE("LodQuadTree: out-of-range sections get disabled and marked dirty, reload when back") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.settle(a);
    std::vector<LodQuadTree::Section> before;
    f.tree->collectEnabled(before);

    // move by less than a root node (1024 blocks) but more than the node-clearing distance (64)
    const BlockPos2D b(300, 0);
    f.tree->tryTick(b);
    int wentOut = 0;
    for (auto& s : before) {
        if (!f.tree->isSectionPosInBounds(s->pos)) {
            wentOut++;
            CHECK_FALSE(s->getRenderingEnabled());
            CHECK(s->renderDataDirty.load());
            CHECK(s->canRender()); // the buffers stay until the section is reloaded or removed
            CHECK_FALSE(f.enabledSet().contains(s->pos));
        }
    }
    CHECK(wentOut > 0);
    f.settle(b);
    checkTiling(f, b, "after small move");

    // coming back: dirty sections are re-uploaded before they render again
    const std::size_t uploadsBefore = f.sink.uploads().size();
    f.settle(a);
    checkTiling(f, a, "back at A");
    CHECK(f.sink.uploads().size() > uploadsBefore);
    for (auto& s : before) CHECK_FALSE((s->renderDataDirty.load() && s->getRenderingEnabled()));
}

TEST_CASE("LodQuadTree: queueReload re-uploads pos and the 4 neighbours; provider listener only enqueues") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.settle(a);

    std::vector<LodQuadTree::Section> enabled;
    f.tree->collectEnabled(enabled);
    REQUIRE_FALSE(enabled.empty());
    // pick an enabled section whose 4 neighbours are enabled too (same detail), if any
    Pos                 target     = enabled.front()->pos;
    const std::set<Pos> enabledSet = f.enabledSet();
    for (auto& s : enabled) {
        bool allAdj = true;
        for (Direction d : kCardinalDirections) allAdj &= enabledSet.contains(sec::adjacent(s->pos, d));
        if (allAdj) {
            target = s->pos;
            break;
        }
    }
    const std::size_t uploadsBefore = f.sink.uploads().size();
    f.tree->queueReload(target);
    f.round(a);
    std::set<Pos> reuploaded;
    for (std::size_t i = uploadsBefore; i < f.sink.uploads().size(); i++)
        reuploaded.insert(f.sink.uploads()[i].sectionPos);
    CHECK(reuploaded.contains(target));
    for (Direction d : kCardinalDirections) {
        const Pos adj = sec::adjacent(target, d);
        if (enabledSet.contains(adj)) CHECK(reuploaded.contains(adj));
    }
    // still renderable and enabled throughout
    CHECK(f.enabledSet().contains(target));

    // a real provider update reaches the tree through attachToProvider (listener enqueues only)
    MemoryLodStore store;
    ManualClock    clock;
    ManualExecutor fileExec;
    SourceProvider provider(store, f.data.reg, clock, fileExec);
    f.tree->attachToProvider(provider);
    const std::size_t uploads2 = f.sink.uploads().size();
    auto              src      = f.data.make(target);
    auto              r        = provider.updater().update(src);
    REQUIRE(r);
    CHECK(*r);
    f.round(a);
    CHECK(f.sink.uploads().size() > uploads2);
    CHECK_EQ(f.sink.uploads()[uploads2].sectionPos, target);
    f.tree->close();
    // after close() a late update must not crash (the listener was removed; a racing one is harmless)
    (void)provider.updater().update(f.data.make(sec::encode(6, 5, 5)));
}

TEST_CASE("LodRenderSection: a load cancelled before it runs never uploads") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.tree->tryTick(a); // loads queued, none run yet
    REQUIRE(f.renderLoader.queueSize() > 0);

    // teleport: all roots fall out of range, every queued section is disposed
    const BlockPos2D b(80'000, 80'000);
    f.tree->tryTick(b);
    const std::size_t queuedForB = f.renderLoader.queueSize();
    f.renderLoader.runAll();
    for (auto& u : f.sink.uploads()) {
        INFO("upload ", sec::toString(u.sectionPos));
        CHECK(f.tree->isSectionPosInBounds(u.sectionPos));
    }
    CHECK(f.sink.uploads().size() <= queuedForB);

    // clearRenderDataCache(true) while loads are queued: sections disposed, their builds skipped
    f.sink.completeAll();
    f.tree->tryTick(b);
    f.tree->clearRenderDataCache(true);
    const std::size_t uploadsBefore = f.sink.uploads().size();
    f.renderLoader.runAll();
    CHECK_EQ(f.sink.uploads().size(), uploadsBefore);

    // a build whose section is disposed between build and upload answer frees the buffer again
    f.settle(b);
    std::vector<LodQuadTree::Section> enabled;
    f.tree->collectEnabled(enabled);
    REQUIRE_FALSE(enabled.empty());
    auto s = enabled.front();
    s->renderDataDirty.store(true);
    REQUIRE(s->uploadRenderDataToGpuAsync());
    CHECK(s->gpuUploadInProgress());
    CHECK_FALSE(s->uploadRenderDataToGpuAsync()); // one build at a time
    f.renderLoader.runAll();
    CHECK(f.sink.pendingCount() == 1);
    // dispose with the upload pending: removeIf(pos, ourId) frees the (pending) buffers right away ...
    const std::size_t removesBefore = f.sink.removes().size();
    s->dispose();
    CHECK_EQ(f.sink.removes().size(), removesBefore + 1);
    CHECK_FALSE(s->gpuUploadInProgress());
    // ... and the late callback's removeIf is stale (nothing newer to remove), never a second remove
    const std::size_t staleBefore = f.sink.staleRemoves().size();
    f.sink.completeAll();
    CHECK_FALSE(s->canRender());
    CHECK_EQ(f.sink.removes().size(), removesBefore + 1);
    CHECK_EQ(f.sink.staleRemoves().size(), staleBefore + 1);
    CHECK_FALSE(s->uploadRenderDataToGpuAsync());
}

TEST_CASE("LodRenderSection: empty data uploads ONCE as an empty section (Java), renders nothing, is requested") {
    // Java: provider.get -> createEmpty, transformer -> sized empty ColumnRenderSource, makeLodRenderData skips
    // every column, LodRenderSection uploads an EMPTY container -> canRender() TRUE (never re-queued).
    FakeRetrievalQueue queue;
    LodQuadTreeDeps    deps;
    deps.retrievalQueue      = &queue;
    deps.positionsToRetrieve = [](Pos pos, std::int8_t) -> std::optional<std::vector<Pos>> {
        return std::vector<Pos>{pos};
    };
    Fx f(32, {0, 0}, std::move(deps));
    f.data.available = std::set<Pos>{}; // nothing has data
    const BlockPos2D a(0, 0);
    f.round(a);
    REQUIRE_FALSE(f.sink.uploads().empty());
    for (auto& u : f.sink.uploads()) {
        CHECK(u.buffers.empty()); // "render nothing" uploads
    }
    const std::size_t uploadsAfterFirst = f.sink.uploads().size();
    const int         calls             = f.data.getCalls;
    f.round(a);
    f.round(a);
    CHECK_EQ(f.sink.uploads().size(), uploadsAfterFirst); // exactly one upload per section
    CHECK_EQ(f.data.getCalls, calls);                     // nothing is re-loaded on later ticks

    // the empty leaf is renderable and enabled (its parent hands over to 4 empty-but-renderable children)
    const Pos leaf = sec::encode(6, 0, 0);
    auto      v    = f.tree->tryGetValue(leaf);
    REQUIRE(v.has_value());
    CHECK((*v)->canRender());
    CHECK((*v)->getRenderingEnabled());
    CHECK_FALSE((*v)->hasOpaque());
    checkTiling(f, a, "empty world still tiles");
    for (auto& e : f.enabledSet()) CHECK((sec::detail(e) == 6 || sec::detail(e) == 7));

    // ... and, being an enableDeleteChildren node, it was submitted for retrieval (world gen / download)
    bool leafRequested = false;
    for (auto& [pos, genDetail] : queue.submitted) leafRequested |= pos == leaf;
    CHECK(leafRequested);
    CHECK(f.tree->canRegenerate()); // only block-detail positions are missing

    // a dropped upload (ok = false) leaves a section unrenderable and the next tick retries it
    const Pos other = sec::encode(6, 3, 3);
    auto      o     = f.tree->tryGetValue(other);
    REQUIRE(o.has_value());
    (*o)->renderDataDirty.store(true);
    f.tree->tryTick(a);
    f.renderLoader.runAll();
    REQUIRE_EQ(f.sink.pendingCount(), 1u);
    f.sink.completeAll(false);
    CHECK((*o)->canRender()); // the old (empty) buffers keep rendering
    CHECK((*o)->renderDataDirty.load());
    CHECK_FALSE((*o)->gpuUploadInProgress());
    f.round(a);
    CHECK_FALSE((*o)->renderDataDirty.load());
}

TEST_CASE("LodQuadTree: a recursing root is never rendered (:676-682)") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.tree->tryTick(a);
    f.renderLoader.runAll();
    // answer only the root uploads: the roots can render but must not be enabled
    for (auto& u : f.sink.uploads()) {
        if (u.onDone && sec::detail(u.sectionPos) == f.tree->treeRootDetailLevel) {
            auto cb  = std::move(u.onDone);
            u.onDone = nullptr;
            cb(true);
        }
    }
    f.tree->tryTick(a);
    CHECK(f.enabledSet().empty());
    auto root = f.tree->tryGetValue(sec::encode(f.tree->treeRootDetailLevel, 0, 0));
    REQUIRE(root.has_value());
    CHECK((*root)->canRender());
    CHECK_FALSE((*root)->getRenderingEnabled());
}

TEST_CASE("LodRenderSection: a disposed section's late upload never wipes the replacement's buffers (b3)") {
    // Batching sink scenario: A uploads (pending), A is disposed, replacement B at the same pos uploads and is
    // applied, then A's upload callback fires ok. Java closed A's own container; here removeIf(pos, A.id) must
    // be ignored because B's upload is newer.
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.settle(a);

    std::vector<LodQuadTree::Section> enabled;
    f.tree->collectEnabled(enabled);
    REQUIRE_FALSE(enabled.empty());
    auto      sectionA = enabled.front();
    const Pos pos      = sectionA->pos;

    // A re-uploads (pending in the batching sink)
    sectionA->renderDataDirty.store(true);
    REQUIRE(sectionA->uploadRenderDataToGpuAsync());
    f.renderLoader.runAll();
    REQUIRE_EQ(f.sink.pendingCount(), 1u);
    const IVertexSink::UploadId idA = f.sink.newestId(pos);

    // A is disposed while its upload is pending: removeIf(pos, idA) frees A's buffers (newest == idA)
    const std::size_t removesBefore = f.sink.removes().size();
    sectionA->dispose();
    CHECK_EQ(f.sink.removes().size(), removesBefore + 1);
    CHECK_EQ(f.sink.newestId(pos), 0u);

    // replacement B at the same pos uploads and is applied
    auto sectionB = std::make_shared<LodRenderSection>(pos, f.tree->shared(), f.ctx);
    REQUIRE(sectionB->uploadRenderDataToGpuAsync());
    f.renderLoader.runAll();
    const IVertexSink::UploadId idB = f.sink.newestId(pos);
    CHECK(idB > idA);
    for (auto& u : f.sink.uploads()) {
        if (u.onDone && u.id == idB) {
            auto cb  = std::move(u.onDone);
            u.onDone = nullptr;
            cb(true);
        }
    }
    CHECK(sectionB->canRender());

    // A's stale callback fires now: it must not remove B's buffers
    const std::size_t removesBeforeStale = f.sink.removes().size();
    f.sink.completeAll(true);
    CHECK_EQ(f.sink.removes().size(), removesBeforeStale);
    CHECK_EQ(f.sink.staleRemoves().size(), 1u);
    CHECK_EQ(f.sink.newestId(pos), idB);
    CHECK(sectionB->canRender());
    CHECK_FALSE(sectionA->canRender());

    // B's own dispose frees B
    sectionB->dispose();
    CHECK_EQ(f.sink.newestId(pos), 0u);
    CHECK_EQ(f.sink.removes().size(), removesBeforeStale + 1);
}

TEST_CASE("LodQuadTree: retrieval queuing is near-to-far, split requeues children, failures requeue") {
    FakeRetrievalQueue queue;
    LodQuadTreeDeps    deps;
    deps.retrievalQueue      = &queue;
    deps.positionsToRetrieve = [](Pos pos, std::int8_t) -> std::optional<std::vector<Pos>> {
        return std::vector<Pos>{pos};
    };
    Fx               f(32, {0, 0}, std::move(deps));
    const BlockPos2D a(0, 0);
    f.settle(a);

    REQUIRE_FALSE(queue.submitted.empty());
    // submitted in manhattan order with the absolute data detail
    std::int32_t last = -1;
    for (auto& [pos, genDetail] : queue.submitted) {
        CHECK_EQ(genDetail, sec::detail(pos) - 6);
        const std::int32_t d = sec::manhattanBlockDistance(pos, a);
        CHECK(d >= last);
        last = d;
    }
    const std::size_t queued = f.tree->queuedRetrievalCount();
    CHECK(queued > 0);
    CHECK_EQ(f.tree->missingRetrievalCount(), 0u);

    // one fails -> back in the missing set; one splits -> its 4 children become missing
    auto it     = queue.futures.begin();
    Pos  failed = it->first;
    it->second->complete(RetrievalResult::failed("boom"));
    ++it;
    Pos split = it->first;
    it->second->complete(RetrievalResult::split());
    CHECK_EQ(f.tree->queuedRetrievalCount(), queued - 2);
    CHECK_EQ(f.tree->missingRetrievalCount(), 5u);

    // the next tick re-submits them (the fake returns the same future for a known pos)
    const std::size_t submittedBefore = queue.submitted.size();
    f.round(a);
    CHECK(queue.submitted.size() >= submittedBefore + 5);
    (void)failed;
    (void)split;

    // teleport: requests inside removed roots are pruned through removeRetrievalRequestIf
    f.settle({90'000, 0});
    CHECK_FALSE(queue.removeIfCalls.empty());
    f.tree->close();
}

TEST_CASE("LodQuadTree: 200 random seeds keep the tiling / teleport invariants") {
    // 16 seeds by default (~8 s under ASan); the full 200 with DH_GOLDEN_SLOW set
    const std::uint32_t kSeedCount = std::getenv("DH_GOLDEN_SLOW") != nullptr ? 200u : 16u;
    for (std::uint32_t seed = 1; seed <= kSeedCount; seed++) {
        std::mt19937 rng(seed);
        INFO("seed ", seed);
        const int radius = 32 + static_cast<int>(rng() % 5); // 32..36 chunks (root grid alignment varies)
        Fx        f(radius);
        f.data.emptyAdjacent = true;

        f.options.client.graphics.quality.horizontalQuality       = static_cast<HorizontalQuality>(rng() % 5);
        f.options.client.graphics.quality.maxHorizontalResolution = static_cast<MaxHorizontalResolution>(rng() % 5);

        auto randomPos = [&]() {
            return BlockPos2D(
                static_cast<std::int32_t>(rng() % 20001) - 10000,
                static_cast<std::int32_t>(rng() % 20001) - 10000
            );
        };
        const BlockPos2D a = randomPos();
        f.settle(a, 80);
        checkTiling(f, a, "seed tiling at A");

        // odd seeds: a small move keeps the tree (partial out-of-bounds); even seeds: a teleport replaces it
        const BlockPos2D b =
            (seed % 2 == 1)
                ? a.add(static_cast<std::int32_t>(rng() % 701) - 350, static_cast<std::int32_t>(rng() % 701) - 350)
                : randomPos().add(60'000, 0);
        const std::set<Pos> atA = f.enabledSet();
        f.settle(b, 80);
        checkTiling(f, b, seed % 2 == 1 ? "seed tiling after small move" : "seed tiling after teleport");
        if (seed % 2 == 0) {
            for (Pos p : atA) CHECK_FALSE(f.tree->tryGetValue(p).has_value()); // teleport: nothing stale
        }
        f.tree->close();
    }
}

//=============//
// render list //
//=============//

TEST_CASE("RenderList: near-to-far, frustum culling, never-cull") {
    Fx               f(32);
    const BlockPos2D a(0, 0);
    f.settle(a);

    std::vector<LodQuadTree::Section> enabled;
    f.tree->collectEnabled(enabled);
    REQUIRE(enabled.size() > 4);

    // no culling: everything, sorted near-to-far by manhattan distance from the tree centre
    RenderList list;
    list.build(enabled, f.tree->getCenterBlockPos(), nullptr, false);
    CHECK_EQ(list.entries().size(), enabled.size());
    CHECK_EQ(list.culledBufferCount(), 0);
    std::int32_t last = -1;
    for (auto& e : list.entries()) {
        const std::int32_t d = sec::centerBlockPos(e.pos).manhattanDist(f.tree->getCenterBlockPos());
        CHECK(d >= last);
        last = d;
        CHECK(e.hasOpaque);
        CHECK_EQ(e.minBlockX, sec::minCornerBlockX(e.pos));
        CHECK_EQ(e.blockWidth, sec::blockWidth(e.pos));
    }

    // a camera at the origin looking towards -Z: nothing with minZ > 0 survives
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.05f, 10000.0f);
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.5f, 80.0f, 0.5f), glm::vec3(0.5f, 80.0f, -100.0f), glm::vec3(0, 1, 0));
    Frustum frustum;
    frustum.update(f.level.minY, f.level.maxY, proj * view);
    list.build(enabled, f.tree->getCenterBlockPos(), &frustum, false);
    CHECK(list.culledBufferCount() > 0);
    CHECK(!list.entries().empty());
    CHECK_EQ(static_cast<std::size_t>(list.visibleBufferCount() + list.culledBufferCount()), enabled.size());
    for (auto& e : list.entries()) {
        CHECK(e.minBlockZ <= 0);
    }
    CHECK_EQ(
        list.vboRenderDebugMenuString(false),
        "VBO Render Count: [" + std::to_string(list.visibleBufferCount()) + "/" + std::to_string(enabled.size()) + "]"
    );

    // the full path: RenderListParams with the DH matrices; shadow pass never culls by default
    RenderListParams params;
    params.options             = &f.options;
    params.worldMinY           = f.level.minY;
    params.worldMaxY           = f.level.maxY;
    params.mcModelViewMatrix   = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -100.0f), glm::vec3(0, 1, 0));
    params.exactCameraPosition = glm::dvec3(0.5, 80.0, 0.5);
    params.dhProjectionMatrix  = proj;
    list.build(*f.tree, params);
    CHECK(list.culledBufferCount() > 0);
    for (auto& e : list.entries()) CHECK(e.minBlockZ <= 0);

    params.isShadowPass = true;
    list.build(*f.tree, params);
    CHECK_EQ(list.shadowCulledBufferCount(), 0);
    CHECK_EQ(list.entries().size(), enabled.size());

    // disableFrustumCulling flag
    params.isShadowPass                                     = false;
    f.options.client.graphics.culling.disableFrustumCulling = true;
    list.build(*f.tree, params);
    CHECK_EQ(list.culledBufferCount(), 0);
    CHECK_EQ(list.entries().size(), enabled.size());

    // disabled / unrenderable sections are skipped even when inside the frustum
    enabled.front()->setRenderingEnabled(false);
    list.build(enabled, f.tree->getCenterBlockPos(), nullptr, false);
    CHECK_EQ(list.entries().size(), enabled.size() - 1);
}

TEST_CASE("Frustum: viewProjectionMatrix matches JOML ordering for a translation") {
    const glm::mat4  mv(1.0f);
    const glm::dvec3 cam(10.0, 20.0, 30.0);
    const glm::mat4  proj = glm::perspective(glm::radians(70.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4  vp   = viewProjectionMatrix(mv, cam, proj);
    const glm::mat4  ref  = proj * glm::translate(glm::mat4(1.0f), -glm::vec3(cam));
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) CHECK(vp[c][r] == doctest::Approx(ref[c][r]).epsilon(1e-5));

    Frustum fr;
    fr.update(-64, 320, vp);
    // a box right in front of the camera (camera looks down -Z) is visible, one behind is not
    CHECK(fr.intersects(5, -40, 8, 3));
    CHECK_FALSE(fr.intersects(5, 100, 8, 3));
}
