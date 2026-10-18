#include <doctest.h>

#include "dh/platform/Direction.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IChunkGenerator.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IFileSystem.h"
#include "dh/platform/ILodStore.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/ITransport.h"
#include "dh/platform/IVertexSink.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/thread/InlineExecutor.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/util/Assert.h"
#include "dh/util/Result.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace dh;

// ---------------------------------------------------------------------------
// Result / Status
// ---------------------------------------------------------------------------

namespace {

Result<int> parsePositive(int v) {
    if (v <= 0) return Error{ErrorCode::InvalidArgument, "not positive"};
    return v;
}

Status checkEven(int v) {
    if (v % 2 != 0) return {ErrorCode::InvalidArgument, "odd"};
    return Status::ok();
}

} // namespace

TEST_CASE("Result holds a value or an error") {
    Result<int> ok = parsePositive(3);
    REQUIRE(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 3);
    CHECK(*ok == 3);
    CHECK(ok.value_or(-1) == 3);

    Result<int> bad = parsePositive(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    CHECK(bad.error().message == "not positive");
    CHECK(bad.value_or(-1) == -1);

    Result<std::string> str = std::string("abc");
    CHECK(str->size() == 3);
    Result<std::string> moved = std::move(str).value();
    CHECK(*moved == "abc");
}

TEST_CASE("Status is Result<void>") {
    Status s = checkEven(2);
    CHECK(s.has_value());
    Status f = checkEven(3);
    CHECK_FALSE(f);
    CHECK(f.error().code == ErrorCode::InvalidArgument);
    CHECK(f.error().message == "odd");
}

// ---------------------------------------------------------------------------
// Assert handler
// ---------------------------------------------------------------------------

namespace {
struct CapturedAssert {
    static inline int         count = 0;
    static inline std::string lastExpr;
    static inline std::string lastMsg;
    static inline int         lastLine = 0;
    static void               handler(const char* expr, const char* msg, const char* file, int line) {
        ++count;
        lastExpr = expr;
        lastMsg  = msg;
        lastLine = line;
        (void)file;
    }
};
} // namespace

TEST_CASE("DH_ASSERT routes to the installed handler and can be restored") {
    AssertHandler previous = setAssertHandler(&CapturedAssert::handler);
    CHECK(previous != &CapturedAssert::handler);
    CHECK(assertHandler() == &CapturedAssert::handler);
    CapturedAssert::count = 0;

    int x = 1;
    DH_ASSERT(x == 1, "fine");
    CHECK(CapturedAssert::count == 0);

    DH_ASSERT(x == 2, "x should be two");
    CHECK(CapturedAssert::count == 1);
    CHECK(CapturedAssert::lastExpr == "x == 2");
    CHECK(CapturedAssert::lastMsg == "x should be two");
    CHECK(CapturedAssert::lastLine > 0);

    // Result::error() on a value is an assertion, not UB.
    Result<int> ok = 5;
    (void)ok.error();
    CHECK(CapturedAssert::count == 2);

#ifdef DH_DEBUG
    DH_DEBUG_ASSERT(false, "debug assert active");
    CHECK(CapturedAssert::count == 3);
#else
    DH_DEBUG_ASSERT(false, "compiled out");
    CHECK(CapturedAssert::count == 2);
#endif

    setAssertHandler(previous);
    CHECK(assertHandler() == previous);
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

namespace {
struct VectorLogger final : ILogger {
    std::vector<std::pair<LogLevel, std::string>> lines;
    void log(LogLevel level, std::string_view message) override { lines.emplace_back(level, std::string(message)); }
};
} // namespace

TEST_CASE("log::set installs a logger and logf formats") {
    VectorLogger logger;
    ILogger*     previous = log::set(&logger);
    log::info("hello");
    log::logf(LogLevel::Warn, "n=%d s=%s", 42, "x");
    log::set(previous);
    log::error("dropped by null logger");

    REQUIRE(logger.lines.size() == 2);
    CHECK(logger.lines[0].first == LogLevel::Info);
    CHECK(logger.lines[0].second == "hello");
    CHECK(logger.lines[1].first == LogLevel::Warn);
    CHECK(logger.lines[1].second == "n=42 s=x");
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

TEST_CASE("ManualClock advances only when told") {
    ManualClock clock(1000);
    CHECK(clock.nowMillis() == 1000);
    std::int64_t n0 = clock.steadyNanos();
    clock.advanceMillis(250);
    CHECK(clock.nowMillis() == 1250);
    CHECK(clock.steadyNanos() - n0 == 250'000'000);

    SystemClock sys;
    CHECK(sys.nowMillis() > 1'600'000'000'000); // after 2020
    std::int64_t a = sys.steadyNanos();
    std::int64_t b = sys.steadyNanos();
    CHECK(b >= a);
}

// ---------------------------------------------------------------------------
// Executors
// ---------------------------------------------------------------------------

TEST_CASE("ManualExecutor is FIFO, drains on demand and rejects after shutdown") {
    ManualExecutor   exec;
    std::vector<int> order;

    CHECK(exec.execute([&] { order.push_back(1); }));
    CHECK(exec.execute([&] {
        order.push_back(2);
        // re-entrant enqueue while running
        exec.execute([&] { order.push_back(4); });
    }));
    CHECK(exec.execute([&] { order.push_back(3); }));
    CHECK(exec.queueSize() == 3);
    CHECK(order.empty());

    CHECK(exec.runOne());
    CHECK(order == std::vector<int>{1});

    // runPending runs only what was queued at call time (2 and 3), not 4
    CHECK(exec.runPending() == 2);
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(exec.queueSize() == 1);

    CHECK(exec.runAll() == 1);
    CHECK(order == std::vector<int>{1, 2, 3, 4});
    CHECK_FALSE(exec.runOne());

    exec.execute([&] { order.push_back(5); });
    exec.shutdown();
    CHECK_FALSE(exec.execute([&] { order.push_back(99); }));
    CHECK(exec.queueSize() == 1); // queued-before-shutdown task still drainable
    CHECK(exec.runAll() == 1);
    CHECK(order.back() == 5);
}

TEST_CASE("InlineExecutor runs synchronously") {
    InlineExecutor exec;
    int            hits = 0;
    CHECK(exec.execute([&] { ++hits; }));
    CHECK(hits == 1);
    CHECK(exec.queueSize() == 0);
    exec.shutdown();
    CHECK_FALSE(exec.execute([&] { ++hits; }));
    CHECK(hits == 1);
}

// ---------------------------------------------------------------------------
// LoopbackTransport
// ---------------------------------------------------------------------------

namespace {
struct RecordingListener final : ITransportListener {
    struct Ev {
        std::string kind;
        SessionId   session;
        std::string payload;
    };
    std::vector<Ev> events;
    ITransport*     replyVia = nullptr; ///< if set, echo frames back with "ack:" prefix

    void onConnected(SessionId s, std::string_view peer) override {
        events.push_back({"connect", s, std::string(peer)});
    }
    void onFrame(SessionId s, std::span<const std::byte> frame) override {
        std::string text(reinterpret_cast<const char*>(frame.data()), frame.size());
        events.push_back({"frame", s, text});
        if (replyVia) {
            std::string reply = "ack:" + text;
            replyVia->send(s, std::as_bytes(std::span<const char>(reply.data(), reply.size())));
        }
    }
    void onDisconnected(SessionId s) override { events.push_back({"disconnect", s, ""}); }
};

std::span<const std::byte> bytesOf(const std::string& s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}
} // namespace

TEST_CASE("LoopbackLink delivers frames FIFO only on pump and enforces maxFrameBytes") {
    LoopbackLink      link(8);
    RecordingListener client, server;
    link.client().setListener(&client);
    link.server().setListener(&server);
    server.replyVia = &link.server();

    SessionId id = link.connect();
    CHECK(link.pending() == 2);
    CHECK(client.events.empty());

    std::string hello = "hi";
    CHECK(link.client().send(id, bytesOf(hello)));
    std::string tooBig = "123456789";
    CHECK_FALSE(link.client().send(id, bytesOf(tooBig)));
    CHECK(link.client().rejectedSends() == 1);
    CHECK_FALSE(link.client().send(999, bytesOf(hello))); // unknown session

    // tick 1: connects + the frame are delivered; the server's reply is queued, not delivered
    CHECK(link.pump() == 3);
    REQUIRE(client.events.size() == 1);
    CHECK(client.events[0].kind == "connect");
    CHECK(client.events[0].payload == "server");
    REQUIRE(server.events.size() == 2);
    CHECK(server.events[0].kind == "connect");
    CHECK(server.events[0].payload == "client");
    CHECK(server.events[1].kind == "frame");
    CHECK(server.events[1].payload == "hi");
    CHECK(link.pending() == 1);

    // tick 2: reply arrives
    CHECK(link.pump() == 1);
    REQUIRE(client.events.size() == 2);
    CHECK(client.events[1].kind == "frame");
    CHECK(client.events[1].payload == "ack:hi");

    // frames queued before a disconnect are still delivered, then both sides see disconnect
    CHECK(link.client().send(id, bytesOf(hello)));
    link.client().disconnect(id);
    CHECK_FALSE(link.client().send(id, bytesOf(hello)));
    CHECK_FALSE(link.isOpen(id));
    link.pumpAll();
    CHECK(server.events.back().kind == "disconnect");
    CHECK(server.events[server.events.size() - 2].kind == "frame");
    CHECK(client.events.back().kind == "disconnect");
    CHECK(client.events.back().session == id);
    CHECK(link.pending() == 0);

    // ids are never reused
    CHECK(link.connect() != id);
}

// ---------------------------------------------------------------------------
// MapBlockRegistry
// ---------------------------------------------------------------------------

TEST_CASE("MapBlockRegistry interns with stable pointers") {
    MapBlockRegistry reg;
    CHECK(reg.air() != nullptr);
    CHECK(reg.air()->isAir);
    CHECK(reg.air()->opacity == 0);
    CHECK(reg.water()->isLiquid);
    CHECK(reg.defaultBiome()->serial == "minecraft:plains");
    CHECK(reg.resolveBlock("minecraft:air") == reg.air());
    CHECK(reg.resolveBlock("minecraft:stone") == nullptr);
    CHECK(reg.resolveBlockOrAir("minecraft:stone") == reg.air());
    CHECK(reg.resolveBiome("minecraft:desert") == nullptr);
    CHECK(reg.resolveBiomeOrDefault("minecraft:desert") == reg.defaultBiome());

    BlockInfo stone;
    stone.serial        = "minecraft:stone";
    stone.material      = BlockMaterial::Stone;
    const BlockInfo* s1 = reg.addBlock(stone);
    const BlockInfo* s2 = reg.addBlock(stone); // duplicate add returns the existing entry
    CHECK(s1 == s2);
    CHECK(reg.resolveBlock("minecraft:stone") == s1);
    CHECK(reg.blockCount() == 3);

    const BiomeInfo* d1 = reg.addBiome(BiomeInfo{"minecraft:desert"});
    CHECK(reg.resolveBiome("minecraft:desert") == d1);
    CHECK(reg.biomeCount() == 2);

    // pointers survive further insertions
    for (int i = 0; i < 100; ++i) {
        BlockInfo b;
        b.serial = "test:block_" + std::to_string(i);
        reg.addBlock(b);
    }
    CHECK(reg.resolveBlock("minecraft:stone") == s1);
    CHECK(s1->material == BlockMaterial::Stone);
}

TEST_CASE("MapBlockRegistry autoCreateUnknown interns on first resolve") {
    MapBlockRegistry reg(true);
    const BlockInfo* a = reg.resolveBlock("mod:thing");
    REQUIRE(a != nullptr);
    CHECK(a->serial == "mod:thing");
    CHECK(a->isSolid);
    CHECK(reg.resolveBlock("mod:thing") == a);
    const BiomeInfo* b = reg.resolveBiome("mod:biome");
    REQUIRE(b != nullptr);
    CHECK(reg.resolveBiome("mod:biome") == b);
}

// ---------------------------------------------------------------------------
// TestChunk heightmaps (Java ChunkWrapper.createDhHeightMaps semantics)
// ---------------------------------------------------------------------------

namespace {
struct Blocks {
    MapBlockRegistry reg;
    const BlockInfo* air   = reg.air();
    const BlockInfo* water = reg.water();
    const BlockInfo* stone;
    const BlockInfo* glass;
    const BlockInfo* torch;
    const BiomeInfo* plains = reg.defaultBiome();

    Blocks() {
        BlockInfo s;
        s.serial   = "minecraft:stone";
        s.material = BlockMaterial::Stone;
        stone      = reg.addBlock(s);

        BlockInfo g; // transparent but solid
        g.serial  = "minecraft:glass";
        g.opacity = 0;
        g.isSolid = true;
        glass     = reg.addBlock(g);

        BlockInfo t; // non-solid, light blocking (opacity 1), emits light
        t.serial        = "minecraft:torch";
        t.opacity       = 1;
        t.isSolid       = false;
        t.lightEmission = 14;
        torch           = reg.addBlock(t);
    }
};
} // namespace

TEST_CASE("TestChunk: empty chunk bounds and heightmaps sit at the build floor") {
    Blocks    b;
    TestChunk chunk(3, -2, -64, 320, b.air, b.plains);
    CHECK(chunk.chunkX() == 3);
    CHECK(chunk.minBlockX() == 48);
    CHECK(chunk.minBlockZ() == -32);
    CHECK(chunk.height() == 384);
    CHECK(chunk.minNonEmptyHeight() == -64);
    // Java quirk: the top-down loop assigns (sectionMin + 16) before testing emptiness and never
    // breaks for an all-empty chunk, so the result is inclusiveMin + 16, not exclusiveMax.
    CHECK(chunk.maxNonEmptyHeight() == -48);
    CHECK(chunk.solidHeightMapValue(0, 0) == -64);
    CHECK(chunk.lightBlockingHeightMapValue(15, 15) == -64);
    CHECK(chunk.blockAt(0, 500, 0) == b.air);
    CHECK(chunk.blockAt(0, -100, 0) == b.air);
    CHECK(chunk.biomeAt(0, 500, 0) == b.plains);
    CHECK(chunk.worldBlockLightPosList().empty());
}

TEST_CASE("TestChunk: non-empty section bounds follow Java section rules") {
    Blocks    b;
    TestChunk chunk(0, 0, -64, 320, b.air, b.plains);
    chunk.setBlock(5, 70, 5, b.stone); // section [64,80)
    chunk.setBlock(5, 10, 5, b.stone); // section [0,16)
    CHECK(chunk.minNonEmptyHeight() == 0);
    CHECK(chunk.maxNonEmptyHeight() == 80);

    chunk.setBlock(0, -64, 0, b.stone); // bottom section
    CHECK(chunk.minNonEmptyHeight() == -64);
    chunk.setBlock(0, 319, 0, b.stone); // top section
    CHECK(chunk.maxNonEmptyHeight() == 320);
}

TEST_CASE("TestChunk: solid vs light-blocking heightmaps") {
    Blocks    b;
    TestChunk chunk(0, 0, -64, 320, b.air, b.plains);
    // column (1,1): stone up to y=59 inclusive, water 60..62, air above
    chunk.fillColumn(1, 1, -64, 60, b.stone);
    chunk.fillColumn(1, 1, 60, 63, b.water);
    CHECK(chunk.solidHeightMapValue(1, 1) == 59);         // water is not solid
    CHECK(chunk.lightBlockingHeightMapValue(1, 1) == 62); // water blocks light (opacity 1)

    // column (2,2): stone to 59, glass at 60 (solid but transparent)
    chunk.fillColumn(2, 2, -64, 60, b.stone);
    chunk.setBlock(2, 60, 2, b.glass);
    CHECK(chunk.solidHeightMapValue(2, 2) == 60);
    CHECK(chunk.lightBlockingHeightMapValue(2, 2) == 59);

    // column (3,3): stone to 59, torch at 60 (non-solid, opacity 1)
    chunk.fillColumn(3, 3, -64, 60, b.stone);
    chunk.setBlock(3, 60, 3, b.torch);
    CHECK(chunk.solidHeightMapValue(3, 3) == 59);
    CHECK(chunk.lightBlockingHeightMapValue(3, 3) == 60);

    // column (4,4): nothing -> both at minNonEmptyHeight (-64, since column (1,1) fills the bottom section)
    CHECK(chunk.minNonEmptyHeight() == -64);
    CHECK(chunk.solidHeightMapValue(4, 4) == -64);
    CHECK(chunk.lightBlockingHeightMapValue(4, 4) == -64);

    // light list in world coords, sorted (y, z, x)
    chunk.setBlock(0, 10, 0, b.torch);
    auto lights = chunk.worldBlockLightPosList();
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].y == 10);
    CHECK(lights[1].x == 3);
    CHECK(lights[1].y == 60);
    CHECK(lights[1].z == 3);
    CHECK(lights[1].level == 14);
}

TEST_CASE("TestChunk: Java quirk - block at minNonEmptyHeight is never examined") {
    Blocks    b;
    TestChunk chunk(0, 0, 0, 64, b.air, b.plains);
    chunk.setBlock(0, 0, 0, b.stone); // only block, at the floor of the only non-empty section
    CHECK(chunk.minNonEmptyHeight() == 0);
    CHECK(chunk.maxNonEmptyHeight() == 16);
    // loop runs while y > 0, so y=0 is skipped and the "not found" value (0) is returned - identical number
    CHECK(chunk.solidHeightMapValue(0, 0) == 0);

    // put the only block at y=16 -> section [16,32): minNonEmpty=16, block at 16 skipped -> 16 either way,
    // but a block at y=17 is found.
    TestChunk chunk2(0, 0, 0, 64, b.air, b.plains);
    chunk2.setBlock(0, 17, 0, b.stone);
    CHECK(chunk2.minNonEmptyHeight() == 16);
    CHECK(chunk2.solidHeightMapValue(0, 0) == 17);
}

TEST_CASE("TestChunk: DH light storage semantics") {
    Blocks    b;
    TestChunk chunk(0, 0, -64, 320, b.air, b.plains);
    CHECK(chunk.dhSkyLight(0, 320, 0) == 15); // above max -> max sky light
    CHECK(chunk.dhSkyLight(0, -65, 0) == 0);
    CHECK(chunk.dhSkyLight(0, 100, 0) == 0);
    CHECK(chunk.dhBlockLight(0, 320, 0) == 0);

    chunk.setDhSkyLight(1, 100, 2, 9);
    chunk.setDhBlockLight(1, 100, 2, 7);
    chunk.setDhBlockLight(1, 999, 2, 7); // ignored
    CHECK(chunk.dhSkyLight(1, 100, 2) == 9);
    CHECK(chunk.dhBlockLight(1, 100, 2) == 7);
    chunk.clearDhBlockLighting();
    CHECK(chunk.dhBlockLight(1, 100, 2) == 0);
    CHECK(chunk.dhSkyLight(1, 100, 2) == 9);
    chunk.clearDhSkyLighting();
    CHECK(chunk.dhSkyLight(1, 100, 2) == 0);

    CHECK_FALSE(chunk.isDhSkyLightingCorrect());
    chunk.setDhSkyLightingCorrect(true);
    CHECK(chunk.isDhSkyLightingCorrect());
}

TEST_CASE("TestChunk: blockBiomeHashCode is deterministic and content-sensitive") {
    Blocks    b;
    TestChunk a(0, 0, -64, 320, b.air, b.plains);
    TestChunk c(0, 0, -64, 320, b.air, b.plains);
    a.fillLayers(-64, 64, b.stone);
    c.fillLayers(-64, 64, b.stone);
    CHECK(a.blockBiomeHashCode() == c.blockBiomeHashCode());
    c.setBlock(7, 63, 7, b.water);
    CHECK(a.blockBiomeHashCode() != c.blockBiomeHashCode());
}

// ---------------------------------------------------------------------------
// RecordingVertexSink
// ---------------------------------------------------------------------------

TEST_CASE("RecordingVertexSink defers callbacks until completed") {
    RecordingVertexSink sink;
    MeshBuffers         buffers;
    buffers.opaque.push_back(std::vector<std::byte>(kQuadBytes));
    int  okCount = 0, failCount = 0;
    auto cb = [&](bool ok) { (ok ? okCount : failCount)++; };

    sink.upload(42, std::move(buffers), cb);
    sink.upload(43, MeshBuffers{}, cb);
    sink.remove(42);
    CHECK(sink.pendingCount() == 2);
    CHECK(okCount == 0);
    CHECK(sink.uploads()[0].buffers.totalBytes() == kQuadBytes);
    CHECK(sink.removes() == std::vector<std::uint64_t>{42});

    CHECK(sink.completeNext(true));
    CHECK(okCount == 1);
    CHECK(sink.completeAll(false) == 1);
    CHECK(failCount == 1);
    CHECK_FALSE(sink.completeNext());

    sink.autoComplete = true;
    sink.upload(44, MeshBuffers{}, cb);
    CHECK(okCount == 2);
}

// ---------------------------------------------------------------------------
// StdFileSystem
// ---------------------------------------------------------------------------

TEST_CASE("StdFileSystem round-trips files without exceptions") {
    StdFileSystem         fs;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "dh_core_platform_test";
    std::error_code       ec;
    std::filesystem::remove_all(dir, ec);

    CHECK_FALSE(fs.exists(dir));
    CHECK(fs.mkdirs(dir / "a" / "b"));
    CHECK(fs.mkdirs(dir / "a" / "b")); // idempotent
    CHECK(fs.exists(dir / "a"));

    std::string            text = "hello\0world";
    std::vector<std::byte> payload(text.size() + 1);
    std::memcpy(payload.data(), text.data(), text.size() + 1);
    auto file = dir / "a" / "data.bin";
    REQUIRE(fs.writeFileAtomic(file, payload));
    CHECK_FALSE(fs.exists(dir / "a" / "data.bin.tmp"));

    auto read = fs.readFile(file);
    REQUIRE(read);
    CHECK(*read == payload);

    auto missing = fs.readFile(dir / "nope");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    auto list = fs.listDir(dir / "a");
    REQUIRE(list);
    REQUIRE(list->size() == 2);
    CHECK((*list)[0].filename() == "b");
    CHECK((*list)[1].filename() == "data.bin");

    CHECK(fs.removeFile(file));
    CHECK(fs.removeFile(file)); // already gone
    CHECK_FALSE(fs.exists(file));
    CHECK_FALSE(fs.removeFile(dir / "a")); // directories refused

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

TEST_CASE("GenerationStep mirrors Java EDhApiWorldGenerationStep") {
    CHECK(static_cast<int>(GenerationStep::DownSampled) == -1);
    CHECK(static_cast<int>(GenerationStep::Light) == 9);
    CHECK(GenerationStep::Terrain == GenerationStep::LiquidCarvers);
    CHECK(std::string(generationStepName(GenerationStep::Terrain)) == "liquid_carvers");
    CHECK(generationStepFromValue(7) == GenerationStep::LiquidCarvers);
    CHECK_FALSE(generationStepFromValue(10).has_value());
    CHECK(generationStepFromName("terrain") == GenerationStep::Terrain);
    CHECK(generationStepFromName("structure_start") == GenerationStep::StructureStart);
    CHECK_FALSE(generationStepFromName("bogus").has_value());
    CHECK(static_cast<int>(GenerationMode::InternalServer) == 6);
}

TEST_CASE("StorePart / StoreFlag bit helpers") {
    StorePart parts = StorePart::Data | StorePart::Meta;
    CHECK(has(parts, StorePart::Data));
    CHECK(has(parts, StorePart::Meta));
    CHECK_FALSE(has(parts, StorePart::Adjacent));
    CHECK(has(StorePart::AllBlobs, StorePart::Mapping));
    CHECK_FALSE(has(StorePart::AllBlobs, StorePart::Meta));

    StoreMeta meta;
    meta.setFlag(StoreFlag::ApplyToParent, true);
    meta.setFlag(StoreFlag::Regenerate, true);
    CHECK(meta.hasFlag(StoreFlag::ApplyToParent));
    CHECK_FALSE(meta.hasFlag(StoreFlag::ApplyToChildren));
    meta.setFlag(StoreFlag::ApplyToParent, false);
    CHECK_FALSE(meta.hasFlag(StoreFlag::ApplyToParent));
    CHECK(meta.hasFlag(StoreFlag::Regenerate));
}

TEST_CASE("Direction mirrors Java EDhDirection ordinals") {
    CHECK(faceIndex(Direction::Down) == 0);
    CHECK(faceIndex(Direction::East) == 5);
    CHECK(compassIndex(Direction::North) == 0);
    CHECK(compassIndex(Direction::East) == 3);
    CHECK(compassIndex(Direction::Up) == -1);
    CHECK(opposite(Direction::West) == Direction::East);
    CHECK(offsetZ(Direction::North) == -1);
    CHECK(kAllDirections[0] == Direction::Up);
}
