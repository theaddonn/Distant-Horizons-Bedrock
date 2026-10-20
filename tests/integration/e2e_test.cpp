// WP-L / plan IT3: end-to-end scenarios over the world/level lifecycle objects.
//
//   1. DhClientServerWorld (singleplayer): load the level through both handles, feed 9x9 hilly chunks through the
//      chunk entry point, tick until quiescent, check the enabled sections tile the view and were all uploaded, walk
//      the player 2 km asserting the render invariants every tick, then unload client -> server -> world.
//   2. DhServerWorld + DhClientWorld over a LoopbackLink (MTU 1500, 500 KB/s): the client requests sections, the
//      server generates them with FakeChunkGenerator, the client's stored sections equal the server's, a real-time
//      update after a chunk change reaches the client.
//   3. Level-load failure: nullptr + exactly one user-facing message, never an exception.
//
// Everything is deterministic: ManualExecutors, ManualClock, LoopbackLink. One "round" = advance the clock, tick the
// world, run every executor to idleness, answer every vertex-sink upload.
#include <doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/gen/FakeChunkGenerator.h"
#include "dh/net/MessageRegistry.h"
#include "dh/net/Messages.h"
#include "dh/platform/IClock.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/world/World.h"
#include "integration/fakes/Fakes.h"

using namespace dh;
using namespace dh::e2e;
using sec::Pos;

namespace {

/// One independent "process": its own executors, sink, options and WorldDeps (client and server of scenario 2 each
/// get one). The clock and the block registry are shared so wire serials resolve to the same interned states.
struct Stack {
    ManualClock&       clock;
    MapBlockRegistry&  reg;
    Options            options = defaultOptions(4);
    ManualPools        pools;
    TrackingVertexSink sink;
    RecordingLogger    logger;
    WorldDeps          deps;
    bool               failStore    = false;
    int                storesOpened = 0;

    Stack(ManualClock& c, MapBlockRegistry& r) : clock(c), reg(r) {
        options.common.multiThreading.numberOfThreads = 2;
        deps.clock                                    = &clock;
        deps.registry                                 = &reg;
        deps.options                                  = &options;
        deps.exec                                     = pools.executors();
        deps.logger                                   = &logger;
        deps.storeFactory                             = [this](ILevelHandle&) -> std::unique_ptr<ILodStore> {
            if (failStore) return nullptr;
            ++storesOpened;
            return std::make_unique<MemoryLodStore>();
        };
        deps.sinkFactory    = [this](ILevelHandle&) -> IVertexSink* { return &sink; };
        deps.drainExecutors = [this] { settle(); };
    }

    /// Runs every executor and answers every upload until nothing is left.
    void settle() {
        for (;;) {
            std::size_t n  = pools.runAll();
            n             += sink.completeAll();
            if (n == 0) break;
        }
    }
    void round(AbstractWorld& world, int ms = 100) {
        clock.advanceMillis(ms);
        world.tick();
        settle();
    }
    bool idle() const { return pools.pending() == 0 && sink.pendingCount() == 0; }
};

//--------------------------------------------------------------------------------------------------------------
// render invariants
//--------------------------------------------------------------------------------------------------------------

using TreePtr = std::shared_ptr<tree::LodQuadTree>;

std::vector<tree::LodQuadTree::Section> enabledSections(const TreePtr& tree) {
    std::vector<tree::LodQuadTree::Section> v;
    tree->collectEnabled(v);
    return v;
}

/// Leaf (detail 6) cells covered by the enabled sections.
std::set<Pos> coverage(const std::vector<tree::LodQuadTree::Section>& enabled) {
    std::set<Pos> cells;
    for (const auto& s : enabled) {
        sec::forEachChildAtDetailLevel(s->pos, 6, [&](Pos leaf) { cells.insert(leaf); });
    }
    return cells;
}

struct TilingResult {
    int cells = 0, holes = 0, overlaps = 0;
};

/// Every in-bounds leaf cell of the tree square is covered by exactly one enabled section.
TilingResult tiling(const TreePtr& tree, const std::vector<tree::LodQuadTree::Section>& enabledList) {
    std::set<Pos> enabled;
    for (const auto& s : enabledList) enabled.insert(s->pos);
    TilingResult       r;
    const std::int32_t half = tree->diameterInBlocks() / 2;
    const BlockPos2D   c    = tree->getCenterBlockPos();
    const Pos          minP = sec::encodeContainingBlock(6, c.x - half, c.z - half);
    const Pos          maxP = sec::encodeContainingBlock(6, c.x + half - 1, c.z + half - 1);
    for (std::int32_t x = sec::x(minP); x <= sec::x(maxP); ++x) {
        for (std::int32_t z = sec::z(minP); z <= sec::z(maxP); ++z) {
            const Pos cell = sec::encode(6, x, z);
            if (!tree->isSectionPosInBounds(cell)) continue;
            ++r.cells;
            int covering = 0;
            for (Pos p = cell;; p = sec::parent(p)) {
                if (enabled.contains(p)) ++covering;
                if (sec::detail(p) >= tree->treeRootDetailLevel) break;
            }
            if (covering == 0) {
                if (r.holes == 0)
                    MESSAGE(
                        "first hole: ",
                        sec::toString(cell),
                        " tree centre ",
                        c.x,
                        ",",
                        c.z,
                        " diameter ",
                        tree->diameterInBlocks()
                    );
                ++r.holes;
            }
            if (covering > 1) ++r.overlaps;
        }
    }
    return r;
}

/// Per-tick invariants. `prev` is the coverage of the previous tick (empty on the first call). Returns the coverage.
std::set<Pos> checkTick(const TreePtr& tree, const TrackingVertexSink& sink, const std::set<Pos>& prev) {
    const auto    enabled = enabledSections(tree);
    std::set<Pos> positions;
    for (const auto& s : enabled) positions.insert(s->pos);

    for (const auto& s : enabled) {
        // no stale enabled nodes: alive, in the tree, rendering, and the sink holds its buffers
        REQUIRE_FALSE(s->isDisposed());
        REQUIRE(s->getRenderingEnabled());
        REQUIRE(s->canRender());
        REQUIRE(tree->isSectionPosInBounds(s->pos));
        REQUIRE(sink.isResident(s->pos));
        // no overlap: no ancestor of an enabled section is enabled too
        for (Pos p = s->pos; sec::detail(p) < tree->treeRootDetailLevel;) {
            p = sec::parent(p);
            REQUIRE_FALSE(positions.contains(p));
        }
    }

    // "parent stays until all four children renderable" => a cell that was covered and is still inside the tree
    // never becomes uncovered between two ticks
    std::set<Pos> now  = coverage(enabled);
    int           lost = 0;
    for (Pos cell : prev) {
        if (tree->isSectionPosInBounds(cell) && !now.contains(cell)) ++lost;
    }
    REQUIRE_EQ(lost, 0);
    REQUIRE_EQ(sink.violations(), 0u);
    return now;
}

/// Rounds until the enabled set is stable, nothing is pending and `done()` holds (3 stable rounds in a row).
template <class Done>
int settleWorld(
    Stack&                          s,
    AbstractWorld&                  world,
    const std::function<TreePtr()>& getTree,
    std::set<Pos>&                  prevCoverage,
    Done                            done,
    int                             maxRounds = 600
) {
    std::set<Pos> lastEnabled;
    TreePtr       lastTree = getTree();
    int           stable = 0, r = 0;
    for (; r < maxRounds; ++r) {
        s.round(world);
        TreePtr tree = getTree();
        REQUIRE(tree);
        if (tree != lastTree) { // a recreated tree starts from nothing: coverage continuity restarts
            prevCoverage.clear();
            lastTree = tree;
        }
        prevCoverage = checkTick(tree, s.sink, prevCoverage);
        std::set<Pos> now;
        for (const auto& sec : enabledSections(tree)) now.insert(sec->pos);
        if (now == lastEnabled && s.idle() && done()) {
            if (++stable >= 3) break;
        } else {
            stable = 0;
        }
        lastEnabled = std::move(now);
    }
    REQUIRE(stable >= 3); // settled within the round budget
    return r;
}

/// TILING-ONLY DEVICE (not a world-gen substitute). Stand-in for "the rest of the world already has LODs" (what world
/// generation or a server download would provide): flat-ish single-data-point columns written through the updater like
/// any other section update. The chunk pipeline is exercised for the area around the player, this keeps the whole view
/// data-bearing so the tiling invariant is meaningful.
struct BackgroundFiller {
    const FakeChunkSource& src;
    Terrain                terrain;
    std::set<Pos>          filled;

    std::size_t fillView(GeneratedProvider& provider, BlockPos2D center, int halfBlocks) {
        std::size_t n  = 0;
        const Pos   lo = sec::encodeContainingBlock(6, center.x - halfBlocks, center.z - halfBlocks);
        const Pos   hi = sec::encodeContainingBlock(6, center.x + halfBlocks, center.z + halfBlocks);
        for (std::int32_t x = sec::x(lo); x <= sec::x(hi); ++x) {
            for (std::int32_t z = sec::z(lo); z <= sec::z(hi); ++z) {
                const Pos pos = sec::encode(6, x, z);
                if (!filled.insert(pos).second) continue;
                auto      section = std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));
                const int stone   = section->mapping.addIfAbsent(terrain.biome, terrain.stone);
                for (int cx = 0; cx < 64; ++cx) {
                    for (int cz = 0; cz < 64; ++cz) {
                        const std::int32_t h =
                            src.heightAt(sec::minCornerBlockX(pos) + cx, sec::minCornerBlockZ(pos) + cz);
                        section->setSingleColumn(
                            {fdp::encode(stone, h + 64, 0, 0, 15)},
                            cx,
                            cz,
                            GenerationStep::Features,
                            WorldCompressionMode::MergeSameBlocks
                        );
                    }
                }
                section->isEmpty = false;
                REQUIRE(provider.updater().update(section).has_value());
                ++n;
            }
        }
        return n;
    }
};

/// No ApplyToParent / ApplyToChildren work is left in the store.
bool propagationDone(AbstractLevel& level) {
    auto up   = level.store().positionsToPropagateUp(0, 0, 1);
    auto down = level.store().positionsToPropagateDown(0, 0, 1);
    return up.has_value() && up->empty() && down.has_value() && down->empty();
}

} // namespace

//===========================================================================================================//
// 1. singleplayer world                                                                                      //
//===========================================================================================================//

TEST_CASE("e2e SP: DhClientServerWorld tiles the view, uploads every section, survives a 2 km walk, unloads cleanly") {
    ManualClock      clock;
    MapBlockRegistry reg;
    const Terrain    terrain = Terrain::from(reg);
    Stack            s(clock, reg);
    s.options.client.graphics.quality.lodChunkRenderDistanceRadius = 24;
    s.options.common.worldGenerator.generatorPlan = GeneratorPlan::Disabled; // chunks come from the game

    FakeLevelHandle server("overworld", false);
    FakeLevelHandle client("overworld", true);
    FakeLevelHandle client2("overworld", true),
        client3("overworld", true); // declared before the world: it notifies them
    client.serverSide = &server;
    BlockPos2D player{8, 8};
    s.deps.playerBlockPos      = [&] { return std::optional<BlockPos2D>(player); };
    ILevelHandle* activeClient = &client;
    s.deps.activeClientLevel   = [&]() -> ILevelHandle* { return activeClient; };

    DhClientServerWorld  world(s.deps);
    DhClientServerLevel* level = world.getOrLoadLevel(server);
    REQUIRE(level != nullptr);
    CHECK(level->ok());
    CHECK_FALSE(level->isRendering());
    DhClientServerLevel* viaClient = world.getOrLoadLevel(client);
    CHECK(viaClient == level); // the client wrapper attaches to the server level
    CHECK(level->isRendering());
    CHECK_EQ(world.clientWrapperCount(server), 1u);
    CHECK_EQ(world.loadedLevelCount(), 1);
    CHECK(world.getLevel(client) == level);
    CHECK(world.getOrLoadLevel(client) == level); // idempotent
    CHECK_EQ(world.clientWrapperCount(server), 1u);

    auto          getTree = [&] { return level->clientside().tree(); };
    std::set<Pos> prevCoverage;

    //----- no data yet: empty sections upload once and render as nothing (Java), so the view already tiles -----
    for (int i = 0; i < 8; ++i) s.round(world);
    {
        const auto t = tiling(getTree(), enabledSections(getTree()));
        CHECK(t.cells > 0);
        CHECK_EQ(t.holes, 0);
        CHECK_EQ(t.overlaps, 0);
    }

    FakeChunkSource   src(terrain, FakeChunkSource::Kind::Hilly);
    ChunkGrid         grid(src);
    BackgroundFiller  background{src, terrain, {}};
    const int         viewHalf      = 24 * 16 + 96; // render radius + margin
    const std::size_t filledInitial = background.fillView(level->provider(), player, viewHalf);
    CHECK(filledInitial > 100);

    //----- feed 9x9 chunks through the chunk entry point -----
    for (int cx = -4; cx <= 4; ++cx)
        for (int cz = -4; cz <= 4; ++cz) grid.get(cx, cz);
    int fed = 0;
    for (int cx = -4; cx <= 4; ++cx) {
        for (int cz = -4; cz <= 4; ++cz) {
            REQUIRE(world.applyChunkUpdate(client, grid.get(cx, cz), grid.neighbours(cx, cz)));
            ++fed;
        }
    }
    CHECK_EQ(fed, 81);
    const int settleRounds =
        settleWorld(s, world, getTree, prevCoverage, [&] { return level->isQuiescent() && propagationDone(*level); });

    CHECK_EQ(level->acceptedChunkUpdates(), 81u);
    CHECK_EQ(level->skippedUnchangedChunks(), 0u);
    CHECK(level->isQuiescent());
    // the data really is in the store (cache flushed -> updater -> store) and its hashes were committed afterwards
    int hashed = 0;
    for (int cx = -4; cx <= 4; ++cx)
        for (int cz = -4; cz <= 4; ++cz) hashed += level->getChunkHash(ChunkPos(cx, cz)) != 0 ? 1 : 0;
    CHECK_EQ(hashed, 81);
    const Pos origin = sec::encodeContainingBlock(6, 8, 8);
    auto      stored = level->provider().get(origin);
    REQUIRE(stored);
    CHECK_FALSE(stored->isEmpty);
    // chunks (-4..4) span 3x3 leaf sections; the parents were produced by the propagator ticks
    CHECK(level->provider().get(sec::parent(origin)) != nullptr);
    CHECK_FALSE(level->provider().get(sec::parent(origin))->isEmpty);

    // tiling + every section uploaded
    TreePtr tree0    = getTree();
    auto    enabled0 = enabledSections(tree0);
    {
        int extra = 0;
        while (tiling(tree0, enabled0).holes > 0 && extra < 80) {
            s.round(world);
            enabled0 = enabledSections(tree0);
            ++extra;
        }
        MESSAGE(
            "DIAG extra rounds to close holes: ",
            extra,
            " holesNow=",
            tiling(tree0, enabled0).holes,
            " queuedLoads renderLoader=",
            s.pools.renderLoader.queueSize(),
            " residents=",
            s.sink.residentCount(),
            " missing=",
            tree0->missingRetrievalCount()
        );
        const auto t = tiling(tree0, enabled0);
        CHECK(t.cells > 0);
        CHECK_EQ(t.holes, 0);
        CHECK_EQ(t.overlaps, 0);
        for (const auto& sct : enabled0) {
            CHECK(s.sink.isResident(sct->pos));
            CHECK(sct->gpuUploadComplete());
        }
        const auto  resident = s.sink.residentSnapshot();
        std::size_t nonEmpty = 0;
        for (const auto& sct : enabled0) {
            const auto it = resident.find(sct->pos);
            if (it != resident.end() && it->second.opaqueBytes > 0) ++nonEmpty;
        }
        CHECK(nonEmpty > 0); // the fed terrain produced geometry
        MESSAGE(
            "SP initial: ",
            enabled0.size(),
            " enabled sections tile ",
            t.cells,
            " leaf cells; ",
            s.sink.residentCount(),
            " resident, ",
            s.sink.totalUploads(),
            " uploads, ",
            s.sink.totalBytes(),
            " bytes, ",
            nonEmpty,
            " sections with geometry, settled in ",
            settleRounds,
            " rounds"
        );
    }

    //----- hash skip: the same chunks again are dropped; a changed chunk is processed -----
    for (int cx = -4; cx <= 4; ++cx)
        for (int cz = -4; cz <= 4; ++cz) world.applyChunkUpdate(client, grid.get(cx, cz), grid.neighbours(cx, cz));
    settleWorld(s, world, getTree, prevCoverage, [&] { return level->isQuiescent() && propagationDone(*level); });
    CHECK_EQ(level->skippedUnchangedChunks(), 81u);
    CHECK_EQ(level->acceptedChunkUpdates(), 81u);

    const std::uint64_t uploadsBefore = s.sink.totalUploads();
    grid.get(0, 0)->fillColumn(3, 3, 80, 120, terrain.stone); // a pillar
    world.applyChunkUpdate(client, grid.get(0, 0), grid.neighbours(0, 0));
    settleWorld(s, world, getTree, prevCoverage, [&] { return level->isQuiescent() && propagationDone(*level); });
    CHECK_EQ(level->acceptedChunkUpdates(), 82u);
    CHECK(s.sink.totalUploads() > uploadsBefore); // provider update listener -> tree reload -> re-upload

    //----- render list -----
    {
        RenderListParams params;
        params.options                                          = s.deps.options;
        s.options.client.graphics.culling.disableFrustumCulling = true;
        const auto& entries                                     = level->clientside().collectRenderList(params);
        CHECK_EQ(entries.size(), enabledSections(getTree()).size());
        for (std::size_t i = 1; i < entries.size(); ++i) { // near to far
            CHECK(entries[i - 1].blockWidth > 0);
        }
    }

    //----- render distance change recreates the tree (ClientLevelModule.clientTick) -----
    {
        const TreePtr before                                           = getTree();
        s.options.client.graphics.quality.lodChunkRenderDistanceRadius = 20;
        world.onOptionsChanged();
        settleWorld(s, world, getTree, prevCoverage, [&] { return getTree() != before; });
        const TreePtr after = getTree();
        CHECK(after != before);
        CHECK_EQ(after->blockRenderDistanceDiameter, 20 * 16 * 2);
        // the old tree was closed: nothing it enabled is rendered any more
        std::vector<tree::LodQuadTree::Section> stale;
        before->collectEnabled(stale);
        for (const auto& sct : stale) CHECK_FALSE(sct->getRenderingEnabled());
    }

    //----- walk 2 km, invariants every tick, full tiling after every step -----
    std::int64_t  walked     = 0;
    std::size_t   minEnabled = SIZE_MAX, maxEnabled = 0;
    std::uint64_t ticksChecked = 0;
    for (int step = 1; step <= 32; ++step) {
        player.x += 64;
        player.z += (step % 2 != 0) ? 16 : -16;
        walked   += 64;
        background.fillView(level->provider(), player, viewHalf);
        if (step % 8 == 0) { // new terrain streams in as the player passes
            const int pcx = player.x >> 4, pcz = player.z >> 4;
            for (int cx = pcx - 4; cx <= pcx + 4; ++cx)
                for (int cz = pcz - 4; cz <= pcz + 4; ++cz) grid.get(cx, cz);
            for (int cx = pcx - 4; cx <= pcx + 4; ++cx)
                for (int cz = pcz - 4; cz <= pcz + 4; ++cz)
                    world.applyChunkUpdate(client, grid.get(cx, cz), grid.neighbours(cx, cz));
        }
        ticksChecked    += static_cast<std::uint64_t>(settleWorld(s, world, getTree, prevCoverage, [&] {
            return level->isQuiescent() && propagationDone(*level);
        }));
        const auto list  = enabledSections(getTree());
        const auto t     = tiling(getTree(), list);
        REQUIRE(t.cells > 0);
        REQUIRE_EQ(t.holes, 0);
        REQUIRE_EQ(t.overlaps, 0);
        minEnabled = std::min(minEnabled, list.size());
        maxEnabled = std::max(maxEnabled, list.size());
        REQUIRE_EQ(getTree()->lastTickPlayerPos().x, player.x);
    }
    CHECK(walked >= 2000);
    CHECK_EQ(s.sink.violations(), 0u);
    CHECK_EQ(level->skippedUnchangedChunks(), 81u); // streamed chunks were all new
    MESSAGE(
        "SP walk: ",
        walked,
        " blocks, ",
        ticksChecked,
        " ticks checked, enabled sections ",
        minEnabled,
        "..",
        maxEnabled,
        ", total uploads ",
        s.sink.totalUploads(),
        " (",
        s.sink.totalBytes(),
        " bytes), removes ",
        s.sink.removes()
    );

    //----- unload order: client wrapper leaves -> renderer stops, server level stays -----
    REQUIRE(s.sink.residentCount() > 0);
    CHECK(world.unloadLevel(client));
    CHECK_EQ(client.unloadCalls, 1);
    CHECK_FALSE(level->isRendering());
    CHECK(world.getLevel(server) == level);
    CHECK_EQ(world.clientWrapperCount(server), 0u);
    CHECK_FALSE(world.unloadLevel(client)); // already gone
    s.settle();
    CHECK_EQ(s.sink.residentCount(), 0u); // every section's buffers were removed
    CHECK_FALSE(level->isClosed());

    // two client wrappers (dimension hopping): the level only stops rendering when the LAST one leaves
    client2.serverSide = client3.serverSide = &server;
    activeClient                            = &client2;
    REQUIRE(world.getOrLoadLevel(client2) == level);
    REQUIRE(world.getOrLoadLevel(client3) == level);
    CHECK_EQ(world.clientWrapperCount(server), 2u);
    CHECK(level->isRendering());
    prevCoverage.clear(); // new tree object after the renderer restart
    settleWorld(s, world, getTree, prevCoverage, [&] { return level->isQuiescent() && propagationDone(*level); });
    CHECK_EQ(tiling(getTree(), enabledSections(getTree())).holes, 0);
    CHECK(world.unloadLevel(client2));
    CHECK(level->isRendering()); // client3 still views it
    CHECK_EQ(world.clientWrapperCount(server), 1u);

    //----- server level unload closes it; nothing is left running -----
    CHECK(world.unloadLevel(server));
    CHECK_EQ(server.unloadCalls, 1);
    CHECK_EQ(world.loadedLevelCount(), 0);
    CHECK(world.getLevel(client3) == nullptr);
    CHECK(s.idle());
    CHECK_EQ(s.sink.residentCount(), 0u);
    CHECK_EQ(s.storesOpened, 1);

    world.close();
    world.close(); // idempotent
    CHECK(world.isClosed());
    CHECK(s.idle());
    CHECK_EQ(s.logger.count(LogLevel::Error), 0u);
}

//===========================================================================================================//
// 2. dedicated server + remote client over a loopback link                                                   //
//===========================================================================================================//

namespace {

struct NetFx {
    ManualClock       clock;
    MapBlockRegistry  reg;
    Terrain           terrain = Terrain::from(reg);
    LoopbackLink      link{1500}; // MTU 1500
    CountingTransport serverWire{link.server()};
    CountingTransport clientWire{link.client()};
    Stack             server{clock, reg};
    Stack             client{clock, reg};
    FakeLevelHandle   serverLevel{"lvl", false};
    FakeLevelHandle   clientLevel{"lvl", true};
    BlockPos2D        player{0, 0};

    std::unique_ptr<mp::ServerPlayers> players;
    std::unique_ptr<mp::ServerNetwork> network;
    std::unique_ptr<DhServerWorld>     sworld;
    std::unique_ptr<DhClientWorld>     cworld;
    SessionId                          sid = 0;

    explicit NetFx(int serverBandwidth = 500, int clientBandwidth = 500, int clientMaxGenDistance = -1) {
        for (Stack* st : {&server, &client}) {
            st->options.common.worldGenerator.generatorPlan            = GeneratorPlan::ChunksOnly;
            st->options.common.worldGenerator.generationCenterChunkX   = -8; // centre block (0,0): see ChunkPos quirk
            st->options.common.worldGenerator.generationCenterChunkZ   = -8;
            st->options.common.worldGenerator.generationMaxChunkRadius = 6;
            st->options.server.playerBandwidthLimit = st == &server ? serverBandwidth : clientBandwidth; // KB/s
            st->options.client.graphics.quality.lodChunkRenderDistanceRadius = 24;
        }
        if (clientMaxGenDistance >= 0) client.options.server.maxGenerationRequestDistance = clientMaxGenDistance;
        server.deps.chunkGeneratorFactory = [this](ILevelHandle&) -> std::unique_ptr<IChunkGenerator> {
            return std::make_unique<FakeChunkGenerator>(
                server.pools.worldGen,
                terrain.air,
                terrain.stone,
                terrain.biome,
                64
            );
        };
        players = std::make_unique<mp::ServerPlayers>(serverWire, clock, serverSettingsFromOptions(server.options));
        network = std::make_unique<mp::ServerNetwork>(*players);
        server.deps.serverPlayers   = players.get();
        server.deps.serverNetwork   = network.get();
        client.deps.clientTransport = &clientWire;
        client.deps.playerBlockPos  = [this] { return std::optional<BlockPos2D>(player); };
        sworld                      = std::make_unique<DhServerWorld>(server.deps);
        cworld                      = std::make_unique<DhClientWorld>(client.deps);
        sid                         = link.connect();
        link.pumpAll();
        sworld->setPlayerPosition(sid, serverLevel, player);
    }

    /// 50 ms: client tick, deliver, server executors, server tick (network 20 Hz), deliver back.
    void round() {
        clock.advanceMillis(50);
        cworld->tick();
        client.settle();
        link.pumpAll();
        server.settle();
        sworld->tick();
        server.settle();
        link.pumpAll();
        client.settle();
    }
    ~NetFx() {
        cworld.reset();
        sworld.reset();
    }
};

} // namespace

TEST_CASE("e2e net: client requests sections, server generates them, stored copies match, real-time update arrives") {
    NetFx fx;
    REQUIRE(fx.cworld->networkState() != nullptr);
    for (int i = 0; i < 4; ++i) fx.round();
    REQUIRE(fx.cworld->networkState()->isReady()); // SessionConfig exchanged
    CHECK_EQ(fx.cworld->networkState()->sessionConfig().playerBandwidthLimit, 500);

    DhServerLevel* srv = fx.sworld->getOrLoadLevel(fx.serverLevel);
    REQUIRE(srv != nullptr);
    CHECK(fx.sworld->getOrLoadLevel(fx.clientLevel) == nullptr); // wrong kind of handle

    // first load is deferred (level key wait), no failure message
    CHECK(fx.cworld->getOrLoadLevel(fx.clientLevel) == nullptr);
    CHECK_EQ(fx.client.logger.count(LogLevel::Error), 0u);
    for (int i = 0; i < 25; ++i) fx.round(); // > 1 s
    DhClientLevel* cli = fx.cworld->getOrLoadLevel(fx.clientLevel);
    REQUIRE(cli != nullptr);
    CHECK(cli->isRendering());

    // Tree-driven retrieval: the (empty, renderable) sections of the client tree ask for what is missing inside the
    // generation window: LodQuadTree.submit -> ProviderRetrievalQueue -> GeneratedProvider::queueRetrieval ->
    // ClientRequestQueue -> server GenQueue -> FakeChunkGenerator. Parents the ChunksOnly server cannot generate come
    // back as Split and the tree re-queues their children.
    for (int i = 0; i < 20 && !cli->requestModule().isWorldGenRunning(); ++i) fx.round();
    REQUIRE(cli->requestModule().isWorldGenRunning());
    REQUIRE(srv->serverside().requestModule().isWorldGenRunning());

    const std::int64_t startMs = fx.clock.nowMillis();
    int                rounds = 0, quiet = 0;
    std::size_t        maxQueued = 0;
    for (; rounds < 4000 && quiet < 12; ++rounds) {
        fx.round();
        auto t = cli->clientside().tree();
        if (!t) continue;
        maxQueued          = std::max(maxQueued, t->queuedRetrievalCount());
        std::size_t leaves = 0;
        if (auto all = cli->store().allPositions())
            for (auto p : *all) leaves += sec::detail(p) == 6 ? 1 : 0;
        const bool done = leaves >= 16
                       && /* out-of-window positions stay "missing" by design */ t->queuedRetrievalCount() == 0
                       && fx.client.idle() && fx.server.idle() && cli->isQuiescent() && srv->isQuiescent();
        quiet = done ? quiet + 1 : 0;
    }
    if (quiet < 12) {
        auto        t      = cli->clientside().tree();
        std::size_t leaves = 0;
        if (auto all = cli->store().allPositions())
            for (auto p : *all) leaves += sec::detail(p) == 6 ? 1 : 0;
        MESSAGE(
            "DIAG leaves=",
            leaves,
            " missing=",
            t ? t->missingRetrievalCount() : 0,
            " queued=",
            t ? t->queuedRetrievalCount() : 0,
            " maxQueued=",
            maxQueued,
            " cIdle=",
            fx.client.idle(),
            " sIdle=",
            fx.server.idle(),
            " cQ=",
            cli->isQuiescent(),
            " sQ=",
            srv->isQuiescent()
        );
    }
    REQUIRE(quiet >= 12);
    CHECK(maxQueued > 0);
    const double simSeconds = static_cast<double>(fx.clock.nowMillis() - startMs) / 1000.0;
    CHECK(fx.client.idle());
    CHECK(fx.server.idle());
    CHECK_EQ(fx.cworld->networkState()->pendingRequestCount(), 0u);

    // compare every detail-6 section the client stored with the server's
    auto clientPositions = cli->store().allPositions();
    auto serverPositions = srv->store().allPositions();
    REQUIRE(clientPositions.has_value());
    REQUIRE(serverPositions.has_value());
    std::size_t compared = 0, withData = 0;
    for (std::uint64_t pos : *clientPositions) {
        if (sec::detail(pos) != 6) continue;
        auto c  = cli->provider().get(pos);
        auto sv = srv->provider().get(pos);
        REQUIRE(c);
        REQUIRE(sv);
        if (!c->contentEquals(*sv)) {
            int colDiff = 0, stepDiff = 0, modeDiff = 0;
            for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
                colDiff  += c->columns[static_cast<std::size_t>(i)] != sv->columns[static_cast<std::size_t>(i)] ? 1 : 0;
                stepDiff += c->columnGenerationStep[static_cast<std::size_t>(i)]
                                 != sv->columnGenerationStep[static_cast<std::size_t>(i)]
                              ? 1
                              : 0;
                modeDiff += c->columnWorldCompressionMode[static_cast<std::size_t>(i)]
                                 != sv->columnWorldCompressionMode[static_cast<std::size_t>(i)]
                              ? 1
                              : 0;
            }
            MESSAGE(
                "MISMATCH ",
                sec::toString(pos),
                " cols=",
                colDiff,
                " steps=",
                stepDiff,
                " modes=",
                modeDiff,
                " c0step=",
                int(c->columnGenerationStep[0]),
                " s0step=",
                int(sv->columnGenerationStep[0]),
                " c0col=",
                c->columns[0].size(),
                " s0col=",
                sv->columns[0].size()
            );
            if (colDiff > 0) {
                const auto& a = c->columns[0];
                const auto& b = sv->columns[0];
                MESSAGE("col0 client ", a.empty() ? 0 : a[0], " server ", b.empty() ? 0 : b[0]);
            }
            CHECK(c->contentEquals(*sv));
        }
        ++compared;
        withData += c->isEmpty ? 0 : 1;
    }
    CHECK(compared >= 16u);
    CHECK_EQ(withData, compared);
    CHECK(serverPositions->size() >= compared);

    // bandwidth + framing
    CHECK_EQ(fx.serverWire.rejected(), 0u);
    CHECK(fx.serverWire.largestFrame() <= 1500);
    CHECK(static_cast<double>(fx.serverWire.bytes()) <= 500'000.0 * simSeconds * 1.05 + 25'000.0);
    MESSAGE(
        "net: ",
        compared,
        " sections generated + downloaded (",
        rounds,
        " rounds, ",
        simSeconds,
        " s simulated); server sent ",
        fx.serverWire.bytes(),
        " bytes in ",
        fx.serverWire.frames(),
        " frames (largest ",
        fx.serverWire.largestFrame(),
        " B, avg ",
        static_cast<double>(fx.serverWire.bytes()) / simSeconds / 1000.0,
        " KB/s); client sent ",
        fx.clientWire.bytes(),
        " bytes"
    );

    // real-time update: a chunk changes on the server, the client's copy follows
    const Pos changedPos = sec::encodeContainingBlock(6, 8, 8);
    auto      before     = cli->provider().get(changedPos);
    REQUIRE(before);
    FakeChunkSource flat(fx.terrain, FakeChunkSource::Kind::Plain);
    auto            chunk = flat.make(0, 0);
    chunk->fillColumn(5, 5, 64, 110, fx.terrain.dirt); // tower on top of the generated ground
    REQUIRE(fx.sworld->applyChunkUpdate(fx.serverLevel, chunk, {}));
    bool arrived = false;
    for (int i = 0; i < 400 && !arrived; ++i) {
        fx.round();
        auto now = cli->provider().get(changedPos);
        arrived  = now && !now->contentEquals(*before);
    }
    REQUIRE(arrived);
    for (int i = 0; i < 40; ++i) fx.round();
    auto cNow = cli->provider().get(changedPos);
    auto sNow = srv->provider().get(changedPos);
    REQUIRE(cNow);
    REQUIRE(sNow);
    CHECK(cNow->contentEquals(*sNow));
    CHECK_FALSE(cNow->contentEquals(*before));

    // shutdown: nothing left behind
    fx.cworld->close();
    fx.sworld->close();
    CHECK(fx.client.idle());
    CHECK(fx.server.idle());
    CHECK_EQ(fx.client.sink.residentCount(), 0u);
}

namespace {

/// Result of one download run, for the assertions of the protocol tests.
struct NetRun {
    DhClientLevel* cli              = nullptr;
    DhServerLevel* srv              = nullptr;
    bool           quiet            = false;
    int            rounds           = 0;
    std::uint64_t  maxDataPerTick   = 0;
    std::uint64_t  maxWirePerTick   = 0;
    std::uint64_t  maxSplitsPerTick = 0;
};

/// Loads both levels and runs until every requested section arrived. Records per-round (= per server 20 Hz tick)
/// payload bytes on the server wire.
NetRun runDownload(NetFx& fx, int maxRounds) {
    NetRun r;
    for (int i = 0; i < 4; ++i) fx.round();
    r.srv = fx.sworld->getOrLoadLevel(fx.serverLevel);
    fx.cworld->getOrLoadLevel(fx.clientLevel);
    for (int i = 0; i < 25; ++i) fx.round();
    r.cli = fx.cworld->getOrLoadLevel(fx.clientLevel);
    if (!r.cli || !r.srv) return r;
    int quiet = 0;
    for (; r.rounds < maxRounds && quiet < 12; ++r.rounds) {
        const auto frames0 = fx.serverWire.log().size();
        const auto bytes0  = fx.serverWire.bytes();
        fx.round();
        std::uint64_t data = 0, splits = 0;
        const auto&   log = fx.serverWire.log();
        for (std::size_t i = frames0; i < log.size(); ++i) {
            if (log[i].id != CountingTransport::kSplitId) continue;
            data += log[i].size - mp::kSplitFrameOverhead;
            ++splits;
        }
        r.maxDataPerTick   = std::max(r.maxDataPerTick, data);
        r.maxSplitsPerTick = std::max(r.maxSplitsPerTick, splits);
        r.maxWirePerTick   = std::max<std::uint64_t>(r.maxWirePerTick, fx.serverWire.bytes() - bytes0);
        auto t = r.cli->clientside().tree();
        if (!t) continue;
        std::size_t leaves = 0;
        if (auto all = r.cli->store().allPositions())
            for (auto p : *all) leaves += sec::detail(p) == 6 ? 1 : 0;
        const bool done = leaves >= 16 && t->queuedRetrievalCount() == 0 && fx.client.idle() && fx.server.idle()
                       && r.cli->isQuiescent() && r.srv->isQuiescent();
        quiet = done ? quiet + 1 : 0;
    }
    r.quiet = quiet >= 12;
    return r;
}

/// Every detail-6 section of the server store is in the client store with identical decoded columns.
std::size_t requireStoresEqual(NetRun& r) {
    auto sp = r.srv->store().allPositions();
    REQUIRE(sp.has_value());
    std::size_t compared = 0;
    for (std::uint64_t pos : *sp) {
        if (sec::detail(pos) != 6) continue;
        auto sv = r.srv->provider().get(pos);
        auto c  = r.cli->provider().get(pos);
        REQUIRE_MESSAGE(sv, sec::toString(pos));
        REQUIRE_MESSAGE(c, "client lacks server section " << sec::toString(pos));
        for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (c->columns[k] != sv->columns[k]) FAIL("column " << i << " differs in " << sec::toString(pos));
        }
        CHECK(c->contentEquals(*sv));
        CHECK_FALSE(c->isEmpty);
        ++compared;
    }
    return compared;
}

/// Splits the server frame log into transfers (maximal runs of FullDataSplit frames) and checks the framing rules.
/// Returns {transfers, transfersWithMoreThanOneSplit, totalSplits, transfersClosedByResponse}.
std::array<std::size_t, 4> checkTransfers(const CountingTransport& wire, std::size_t mtu) {
    const auto&                log = wire.log();
    std::array<std::size_t, 4> out{0, 0, 0, 0};
    for (std::size_t i = 0; i < log.size();) {
        if (log[i].id != CountingTransport::kSplitId) {
            ++i;
            continue;
        }
        std::size_t j = i, firsts = 0;
        for (; j < log.size() && log[j].id == CountingTransport::kSplitId; ++j) {
            firsts += log[j].first ? 1 : 0;
            CHECK(log[j].size <= mtu);
        }
        CHECK_EQ(firsts, 1u); // exactly one isFirst, and it opens the run
        CHECK(log[i].first);
        REQUIRE(j < log.size());
        // the closing message is the FullDataSourceResponse (8) of a request, or the FullDataPartialUpdate (9) of a
        // real-time update (the generated chunks are pushed to the player too)
        CHECK((log[j].id == 8 || log[j].id == 9));
        out[3] += log[j].id == 8 ? 1 : 0;
        ++out[0];
        out[1] += j - i > 1 ? 1 : 0;
        out[2] += j - i;
        i = j;
    }
    return out;
}

} // namespace

TEST_CASE("e2e net protocol: handshake, payload splitting at the frame limit, bandwidth cap, stores equal") {
    // client asks 800 KB/s + generation distance 2000; server allows 300 KB/s + 4096 -> min of each side
    NetFx fx(300, 800, 2000);
    for (int i = 0; i < 4; ++i) fx.round();

    // 1. handshake: SessionConfig(client wanted) -> constrained SessionConfig(server) + LevelInit for the dimension
    REQUIRE(fx.cworld->networkState()->isReady());
    const auto cfg = fx.cworld->networkState()->sessionConfig();
    CHECK_EQ(cfg.playerBandwidthLimit, 300);          // min(800, 300)
    CHECK_EQ(cfg.maxGenerationRequestDistance, 2000); // min(2000, 4096)
    CHECK_EQ(cfg.generationMaxChunkRadius, 6);        // the server's bounds win
    CHECK_EQ(cfg.generationCenterChunkX, -8);
    CHECK(cfg.generatorPlan == net::GeneratorPlan::ChunksOnly);
    CHECK(cfg.enableRealTimeUpdates);
    CHECK(fx.cworld->networkState()->serverSupport() == mp::ServerSupport::Full);
    {
        bool sawClientConfig = false; // the client sent its wanted values, not the constrained ones
        for (const auto& f : fx.clientWire.log()) {
            if (f.id != net::MsgIds::SessionConfig) continue;
            auto m = net::decodeFrame(f.frame);
            REQUIRE(m.has_value());
            const auto& v = static_cast<net::SessionConfigMessage&>(**m).config;
            CHECK_EQ(v.playerBandwidthLimit, 800);
            CHECK_EQ(v.maxGenerationRequestDistance, 2000);
            sawClientConfig = true;
        }
        CHECK(sawClientConfig);
        bool sawServerConfig = false; // the server answered with the constrained values
        for (const auto& f : fx.serverWire.log()) {
            if (f.id != net::MsgIds::SessionConfig) continue;
            auto m = net::decodeFrame(f.frame);
            REQUIRE(m.has_value());
            CHECK(static_cast<net::SessionConfigMessage&>(**m).config == cfg);
            sawServerConfig = true;
        }
        CHECK(sawServerConfig);
    }
    NetRun run = runDownload(fx, 6000);
    REQUIRE(run.cli != nullptr);
    REQUIRE(run.srv != nullptr);
    {
        std::size_t inits = 0;
        for (const auto& f : fx.serverWire.log()) {
            if (f.id != net::MsgIds::LevelInit) continue;
            auto m = net::decodeFrame(f.frame);
            REQUIRE(m.has_value());
            const auto& li = static_cast<net::LevelInitMessage&>(**m);
            CHECK_EQ(li.dimensionResourceLocation, std::string("minecraft:overworld"));
            CHECK_EQ(li.levelKey, fx.serverLevel.levelKey());
            CHECK_EQ(li.levelKey, std::string("lvl"));
            CHECK(net::isValidLevelKey(li.levelKey));
            ++inits;
        }
        CHECK_EQ(inits, 1u); // once per client level object
    }
    REQUIRE(run.quiet);
    CHECK_EQ(fx.cworld->networkState()->pendingRequestCount(), 0u);

    // 2. payload splitting: MTU 1500 -> FullDataSplit frames of at most 1500 B, each run closed by the response
    CHECK_EQ(fx.serverWire.rejected(), 0u);
    CHECK_EQ(fx.clientWire.rejected(), 0u);
    CHECK(fx.serverWire.largestFrame() <= 1500);
    const auto tr = checkTransfers(fx.serverWire, 1500);
    CHECK(tr[0] >= 16u);
    CHECK(tr[1] >= 1u); // at least one response needed several FullDataSplit frames
    CHECK(tr[3] >= 16u); // one payload response per requested section
    CHECK(fx.clientWire.countId(net::MsgIds::FullDataSourceRequest) >= tr[3]);
    // 300 KB/s = 15001 data bytes per 20 Hz tick, plus one frame of slack for the wire total
    CHECK(run.maxDataPerTick <= 300u * 1000 / 20 + 1);
    CHECK(run.maxWirePerTick <= 300u * 1000 / 20 + 1 + 1500 + 13u * run.maxSplitsPerTick);

    // 3. receive: every server section arrived and decodes to the same columns
    const std::size_t compared = requireStoresEqual(run);
    CHECK(compared >= 16u);

    MESSAGE(
        "protocol: rounds=", run.rounds, " serverFrames=", fx.serverWire.frames(), " splitFrames=", tr[2],
        " transfers=", tr[0], " multiSplitTransfers=", tr[1], " maxDataPerTick=", run.maxDataPerTick,
        " maxWirePerTick=", run.maxWirePerTick, " maxSplitsPerTick=", run.maxSplitsPerTick, " sections=", compared
    );
    fx.cworld->close();
    fx.sworld->close();
}

TEST_CASE("e2e net protocol: a tiny bandwidth limit stretches transfers over many ticks but never exceeds it") {
    NetFx fx(48, 500); // 48 KB/s -> 2401 data bytes per tick; client asks more, min wins
    for (int i = 0; i < 4; ++i) fx.round();
    REQUIRE(fx.cworld->networkState()->isReady());
    CHECK_EQ(fx.cworld->networkState()->sessionConfig().playerBandwidthLimit, 48);
    NetRun run = runDownload(fx, 20000);
    REQUIRE(run.cli != nullptr);
    REQUIRE(run.quiet);
    CHECK(run.maxDataPerTick <= 48u * 1000 / 20 + 1);
    CHECK(run.maxWirePerTick <= 48u * 1000 / 20 + 1 + 1500 + 13u * run.maxSplitsPerTick);
    CHECK(run.maxDataPerTick > 1500u); // the cap, not the idle link, is what limits it
    const auto tr = checkTransfers(fx.serverWire, 1500);
    CHECK(tr[1] >= 1u);
    CHECK(requireStoresEqual(run) >= 16u);
    MESSAGE(
        "tiny limit: rounds=", run.rounds, " splitFrames=", tr[2], " transfers=", tr[0],
        " maxDataPerTick=", run.maxDataPerTick, " maxWirePerTick=", run.maxWirePerTick
    );
    fx.cworld->close();
    fx.sworld->close();
}

//===========================================================================================================//
// 3. level load failure                                                                                      //
//===========================================================================================================//

TEST_CASE("e2e failure: a level that cannot load gives nullptr and one user message, never an exception") {
    ManualClock      clock;
    MapBlockRegistry reg;
    Stack            s(clock, reg);
    FakeLevelHandle  h("broken", false);

    SUBCASE("dedicated server: store cannot be opened") {
        s.failStore = true;
        DhServerWorld world(s.deps);
        CHECK(world.getOrLoadLevel(h) == nullptr);
        CHECK_EQ(s.logger.count(LogLevel::Error), 1u);
        CHECK(world.getOrLoadLevel(h) == nullptr); // retried, but the user is told only once
        CHECK_EQ(s.logger.count(LogLevel::Error), 1u);
        CHECK_EQ(world.loadedLevelCount(), 0);
        CHECK_FALSE(world.unloadLevel(h));
        CHECK(s.logger.entries.back().text.find("broken") != std::string::npos);

        s.failStore          = false; // recovered: loads, and a LATER failure is reported again
        DhServerLevel* level = world.getOrLoadLevel(h);
        REQUIRE(level != nullptr);
        CHECK_EQ(world.loadedLevelCount(), 1);
        CHECK(world.unloadLevel(h));
        s.failStore = true;
        CHECK(world.getOrLoadLevel(h) == nullptr);
        CHECK_EQ(s.logger.count(LogLevel::Error), 2u);
    }
    SUBCASE("singleplayer: platform chunk generator cannot be created") {
        s.deps.chunkGeneratorFactory = [](ILevelHandle&) -> std::unique_ptr<IChunkGenerator> { return nullptr; };
        DhClientServerWorld world(s.deps);
        CHECK(world.getOrLoadLevel(h) == nullptr);
        CHECK_EQ(s.logger.count(LogLevel::Error), 1u);
        CHECK_EQ(world.loadedLevelCount(), 0);
        CHECK(s.idle());
    }
    SUBCASE("incomplete dependencies") {
        WorldDeps bad = s.deps;
        bad.registry  = nullptr;
        DhServerWorld world(bad);
        CHECK(world.getOrLoadLevel(h) == nullptr);
        CHECK_EQ(s.logger.count(LogLevel::Error), 1u);
    }
    SUBCASE("client world: wrong handle kind is not a failure") {
        DhClientWorld world(s.deps);
        CHECK(world.getOrLoadLevel(h) == nullptr);
        CHECK_EQ(s.logger.count(LogLevel::Error), 0u);
    }
}

TEST_CASE("WorldTicker ticks at most every period") {
    ManualClock      clock;
    MapBlockRegistry reg;
    Stack            s(clock, reg);
    DhServerWorld    world(s.deps);
    WorldTicker      ticker(world, clock, 100);
    CHECK(ticker.poll());
    clock.advanceMillis(99);
    CHECK_FALSE(ticker.poll());
    clock.advanceMillis(1);
    CHECK(ticker.poll());
    clock.advanceMillis(1000);
    CHECK(ticker.poll());
    CHECK_FALSE(ticker.poll()); // no catch-up burst
    CHECK_EQ(ticker.tickCount(), 3u);
}

TEST_CASE("WorldTicker keeps its rate under jitter (polls slightly faster than the period)") {
    ManualClock      clock;
    MapBlockRegistry reg;
    Stack            s(clock, reg);
    DhServerWorld    world(s.deps);
    WorldTicker      ticker(world, clock, 50);
    for (int i = 0; i < 100; ++i) {
        ticker.poll();
        clock.advanceNanos(49'900'000); // 49.9 ms
    }
    // 5 s at 20 Hz = 100 ticks; dropping the remainder per tick would give 50
    CHECK(ticker.tickCount() >= 98u);
    CHECK(ticker.tickCount() <= 101u);
    // a stall resyncs instead of bursting
    clock.advanceMillis(10'000);
    const auto before = ticker.tickCount();
    CHECK(ticker.poll());
    CHECK_FALSE(ticker.poll());
    CHECK_EQ(ticker.tickCount(), before + 1);
}

TEST_CASE("e2e save: a rejected save never commits the chunk hash (edit is retried, not lost)") {
    ManualClock      clock;
    MapBlockRegistry reg;
    const Terrain    terrain = Terrain::from(reg);
    Stack            s(clock, reg);
    s.options.common.worldGenerator.generatorPlan = GeneratorPlan::Disabled;
    FakeLevelHandle h("lvl", false);
    DhServerWorld   world(s.deps);
    DhServerLevel*  level = world.getOrLoadLevel(h);
    REQUIRE(level != nullptr);

    FakeChunkSource src(terrain, FakeChunkSource::Kind::Plain);
    auto            chunk = src.make(0, 0);
    REQUIRE(world.applyChunkUpdate(h, chunk, {}));
    for (int i = 0; i < 30; ++i) s.round(world);
    const std::int32_t oldHash = static_cast<std::int32_t>(chunk->blockBiomeHashCode());
    REQUIRE_EQ(level->getChunkHash(ChunkPos(0, 0)), oldHash);

    chunk->fillColumn(5, 5, 64, 90, terrain.dirt);
    const std::int32_t newHash = static_cast<std::int32_t>(chunk->blockBiomeHashCode());
    REQUIRE(newHash != oldHash);
    REQUIRE(world.applyChunkUpdate(h, chunk, {}));
    s.pools.lodBuilder.runAll(); // merged into the delayed save cache
    s.pools.file.shutdown();     // the update executor refuses from now on
    for (int i = 0; i < 30; ++i) s.round(world);

    CHECK_EQ(level->getChunkHash(ChunkPos(0, 0)), oldHash); // NOT newHash: the data was never written
    CHECK(level->isQuiescent());
    world.close();
    CHECK(s.idle());
}

TEST_CASE("e2e close: a level closes safely with tasks queued and no drain hook") {
    ManualClock      clock;
    MapBlockRegistry reg;
    const Terrain    terrain = Terrain::from(reg);
    Stack            s(clock, reg);
    s.deps.drainExecutors                         = {};
    s.deps.closeWaitMs                            = 30;
    s.options.common.worldGenerator.generatorPlan = GeneratorPlan::Disabled;
    FakeLevelHandle h("lvl", false);
    {
        DhServerWorld  world(s.deps);
        DhServerLevel* level = world.getOrLoadLevel(h);
        REQUIRE(level != nullptr);
        FakeChunkSource src(terrain, FakeChunkSource::Kind::Plain);
        auto            chunk = src.make(0, 0);
        REQUIRE(world.applyChunkUpdate(h, chunk, {})); // task stays queued on the manual lod-builder
        s.round(world);
        world.close(); // must not touch freed memory when the queued task finally runs
    }
    s.pools.runAll(); // late tasks find a dead gate / leaked storage objects
    CHECK(s.pools.pending() == 0);
}

TEST_CASE("e2e SP generation: GenQueue -> FakeChunkGenerator -> reload, generated area tiles and renders") {
    ManualClock      clock;
    MapBlockRegistry reg;
    const Terrain    terrain = Terrain::from(reg);
    Stack            s(clock, reg);
    s.options.client.graphics.quality.lodChunkRenderDistanceRadius = 24;
    s.options.common.worldGenerator.generatorPlan                  = GeneratorPlan::ChunksOnly;
    s.options.common.worldGenerator.generationCenterChunkX         = -8; // centre block (0,0)
    s.options.common.worldGenerator.generationCenterChunkZ         = -8;
    s.options.common.worldGenerator.generationMaxChunkRadius       = 6;
    s.deps.chunkGeneratorFactory = [&](ILevelHandle&) -> std::unique_ptr<IChunkGenerator> {
        return std::make_unique<FakeChunkGenerator>(s.pools.worldGen, terrain.air, terrain.stone, terrain.biome, 64);
    };
    FakeLevelHandle server("overworld", false), client("overworld", true);
    client.serverSide = &server;
    BlockPos2D player{0, 0};
    s.deps.playerBlockPos    = [&] { return std::optional<BlockPos2D>(player); };
    s.deps.activeClientLevel = [&]() -> ILevelHandle* { return &client; };

    DhClientServerWorld  world(s.deps);
    DhClientServerLevel* level = world.getOrLoadLevel(server);
    REQUIRE(level != nullptr);
    REQUIRE(world.getOrLoadLevel(client) == level);
    auto getTree = [&] { return level->clientside().tree(); };

    // NO BackgroundFiller: every section with data comes out of the generator through the tree's own requests
    int quiet = 0, rounds = 0;
    for (; rounds < 3000 && quiet < 12; ++rounds) {
        s.round(world, 100);
        auto        t      = getTree();
        std::size_t leaves = 0;
        if (auto all = level->store().allPositions())
            for (auto p : *all) leaves += sec::detail(p) == 6 ? 1 : 0;
        const bool done =
            t && leaves >= 16 && /* out-of-window positions stay "missing" by design */ t->queuedRetrievalCount() == 0
            && s.idle() && level->isQuiescent() && propagationDone(*level) && level->serverside().genQueue() != nullptr
            && level->serverside().genQueue()->waitingTaskCount() == 0
            && level->serverside().genQueue()->inProgressTaskCount() == 0;
        quiet = done ? quiet + 1 : 0;
    }
    if (quiet < 12) {
        auto        t  = getTree();
        std::size_t lv = 0;
        if (auto all = level->store().allPositions())
            for (auto p : *all) lv += sec::detail(p) == 6 ? 1 : 0;
        auto* gq = level->serverside().genQueue();
        MESSAGE(
            "DIAG leaves=",
            lv,
            " missing=",
            t ? t->missingRetrievalCount() : 0,
            " queued=",
            t ? t->queuedRetrievalCount() : 0,
            " idle=",
            s.idle(),
            " quiescent=",
            level->isQuiescent(),
            " prop=",
            propagationDone(*level),
            " genQ=",
            gq != nullptr,
            " waiting=",
            gq ? gq->waitingTaskCount() : 0,
            " inprog=",
            gq ? gq->inProgressTaskCount() : 0,
            " should=",
            level->shouldDoWorldGen()
        );
    }
    REQUIRE(quiet >= 12);

    // generated sections hold real data
    std::size_t leaves = 0, nonEmpty = 0;
    const auto  storedPositions = level->store().allPositions();
    REQUIRE(storedPositions.has_value());
    for (auto p : *storedPositions) {
        if (sec::detail(p) != 6) continue;
        ++leaves;
        nonEmpty += level->provider().get(p)->isEmpty ? 0 : 1;
    }
    CHECK(leaves >= 16);
    CHECK_EQ(nonEmpty, leaves);

    // sky light: every stored section (any detail) must have lit top datapoints - a section whose top datapoints are
    // all sky 0 renders pitch black (StageAVertex lightFactor). Generated sections go through the updater's detail-6
    // bake.
    {
        std::map<int, std::pair<int, int>> darkByDetail; // detail -> {sections, dark}
        for (auto p : *storedPositions) {
            auto src = level->provider().get(p);
            if (!src || src->isEmpty) continue;
            int lit = 0, cols = 0;
            for (const auto& col : src->columns) {
                if (col.empty()) continue;
                ++cols;
                if (fdp::skyLight(col.front()) > 0) ++lit;
            }
            auto& e = darkByDetail[sec::detail(p)];
            ++e.first;
            if (cols > 0 && lit == 0) ++e.second;
        }
        for (const auto& [d, e] : darkByDetail) {
            MESSAGE("detail ", d, ": ", e.first, " sections, ", e.second, " dark");
            CHECK_EQ(e.second, 0);
        }
    }

    // the view tiles and the generated area renders geometry (reload after onWorldGenTaskComplete / provider update)
    const auto enabled = enabledSections(getTree());
    const auto t       = tiling(getTree(), enabled);
    CHECK(t.cells > 0);
    CHECK_EQ(t.holes, 0);
    CHECK_EQ(t.overlaps, 0);
    const auto  resident     = s.sink.residentSnapshot();
    std::size_t withGeometry = 0;
    for (const auto& sct : enabled) {
        auto it = resident.find(sct->pos);
        if (it != resident.end() && it->second.opaqueBytes > 0) ++withGeometry;
    }
    CHECK(withGeometry > 0);
    {
        int dark = 0;
        for (const auto& sct : enabled) dark += sct->renderDark() ? 1 : 0; // HUD render.darkSections
        CHECK_EQ(dark, 0);
    }
    CHECK_EQ(s.sink.violations(), 0u);
    MESSAGE(
        "SP generation: ",
        leaves,
        " leaf sections generated in ",
        rounds,
        " rounds; ",
        enabled.size(),
        " enabled sections tile ",
        t.cells,
        " cells, ",
        withGeometry,
        " with geometry, ",
        s.sink.totalUploads(),
        " uploads"
    );

    world.close();
    CHECK(s.idle());
}
