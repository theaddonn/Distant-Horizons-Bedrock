// Client-side multiplayer modes: DhClientWorld against a silent server (client-only fallback), an answering server
// (server mode, LevelInit key) and a server that answers only after the fallback (late reply).
//
// A "round" = advance the manual clock, tick the world, run every executor, deliver the loopback frames.
#include <doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dh/net/MessageRegistry.h"
#include "dh/net/Messages.h"
#include "dh/platform/IClock.h"
#include "dh/platform/ITransport.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/world/World.h"
#include "integration/fakes/Fakes.h"

using namespace dh;
using namespace dh::e2e;

namespace {

/// Client handle whose key can be set later (what ClientLevelHandle does in the game).
class KeyedHandle final : public ILevelHandle {
public:
    explicit KeyedHandle(std::string dimension = "minecraft:overworld") : id_(dimension) { info_.name = dimension; }
    std::string_view  dhIdentifier() const override { return id_; }
    const ILevelInfo& info() const override { return info_; }
    bool              isClientSide() const override { return true; }
    IClientLevel*     clientLevel() override { return &info_; }
    bool              isServerKeyed() const override { return keyed; }
    void              setKey(const std::string& k) {
        key   = k;
        keyed = true;
    }
    bool        keyed = false;
    std::string key;

private:
    std::string     id_;
    TestClientLevel info_;
};

/// Minimal DH server end: answers like ServerNetwork would (config on demand, LevelInit on request).
struct StubServer final : ITransportListener {
    LoopbackTransport& wire;
    SessionId          sid          = 1; // the first LoopbackLink session
    int                requestInits = 0, configs = 0, otherFrames = 0;
    bool               answerInits = false;

    explicit StubServer(LoopbackTransport& w) : wire(w) { wire.setListener(this); }
    ~StubServer() override { wire.setListener(nullptr); }

    void sendConfig() {
        net::SessionConfigMessage m;
        wire.send(sid, net::encodeFrame(m));
    }
    void sendLevelInit(const std::string& dim, const std::string& key) {
        net::LevelInitMessage m;
        m.dimensionResourceLocation = dim;
        m.levelKey                  = key;
        wire.send(sid, net::encodeFrame(m));
    }
    void onConnected(SessionId s, std::string_view) override { sid = s; }
    void onDisconnected(SessionId) override {}
    void onFrame(SessionId, std::span<const std::byte> frame) override {
        auto d = net::decodeFrame(frame);
        if (!d) return;
        switch ((*d)->id()) {
        case net::MsgIds::SessionConfig:
            ++configs;
            break;
        case net::MsgIds::RequestLevelInit:
            ++requestInits;
            if (answerInits)
                sendLevelInit(static_cast<net::RequestLevelInitMessage&>(**d).dimensionResourceLocation, "srv_key");
            break;
        default:
            ++otherFrames;
        }
    }
};

struct ModeFx {
    ManualClock                    clock;
    MapBlockRegistry               reg;
    Options                        options = defaultOptions(4);
    ManualPools                    pools;
    TrackingVertexSink             sink;
    RecordingLogger                logger;
    WorldDeps                      deps;
    LoopbackLink                   link{1500};
    CountingTransport              wire{link.client()};
    KeyedHandle                    level;
    std::unique_ptr<DhClientWorld> world;
    std::vector<std::string>       initKeys;

    ModeFx() {
        options.common.multiThreading.numberOfThreads = 2;
        deps.clock                                    = &clock;
        deps.registry                                 = &reg;
        deps.options                                  = &options;
        deps.exec                                     = pools.executors();
        deps.logger                                   = &logger;
        deps.storeFactory = [](ILevelHandle&) -> std::unique_ptr<ILodStore> {
            return std::make_unique<MemoryLodStore>();
        };
        deps.sinkFactory     = [this](ILevelHandle&) -> IVertexSink* { return &sink; };
        deps.drainExecutors  = [this] { settle(); };
        deps.clientTransport = &wire;
        deps.playerBlockPos  = [] { return std::optional<BlockPos2D>(BlockPos2D{0, 0}); };
        world                = std::make_unique<DhClientWorld>(deps);
        world->networkState()->setLevelInitHandler([this](const net::LevelInitMessage& m) {
            initKeys.push_back(m.levelKey);
            level.setKey(m.levelKey);
        });
        link.connect();
        link.pumpAll();
    }
    ~ModeFx() { world.reset(); }

    void settle() {
        for (;;) {
            std::size_t n  = pools.runAll();
            n             += sink.completeAll();
            if (n == 0) break;
        }
    }
    void round(int ms = 100) {
        clock.advanceMillis(ms);
        world->tick();
        settle();
        link.pumpAll();
        settle();
    }
    void rounds(int totalMs) {
        for (int t = 0; t < totalMs; t += 100) round();
    }
    mp::ClientNetworkState& net() { return *world->networkState(); }
};

} // namespace

TEST_CASE("client modes: a silent server -> client-only after the timeout, local key, no request traffic") {
    ModeFx fx;
    CHECK(fx.net().dhMode() == mp::DhMode::Pending);
    fx.rounds(1000);
    CHECK(fx.net().dhMode() == mp::DhMode::Pending);
    const auto framesAtConnect = fx.wire.frames();
    CHECK_EQ(framesAtConnect, 1u); // our SessionConfig only

    fx.rounds(mp::ClientNetworkState::kServerReplyTimeoutMs);
    REQUIRE(fx.net().dhMode() == mp::DhMode::ClientOnly);
    CHECK_EQ(fx.net().modeReason(), "no DH reply");
    CHECK_FALSE(fx.net().isReady());

    // the level loads on the FIRST attempt: no LevelInit wait, no LevelInit request
    DhClientLevel* lvl = fx.world->getOrLoadLevel(fx.level);
    REQUIRE(lvl != nullptr);
    CHECK_FALSE(fx.level.keyed);
    CHECK(lvl->isRendering());
    for (int i = 0; i < 100; ++i) fx.round();
    CHECK_EQ(fx.wire.frames(), framesAtConnect); // no requests, no RequestLevelInit
    CHECK_EQ(fx.net().sectionsReceived(), 0u);
    CHECK(fx.world->getLevel(fx.level) == lvl);
}

TEST_CASE("client modes: an answering server -> server mode, the LevelInit key is used") {
    ModeFx     fx;
    StubServer srv(fx.link.server());
    srv.answerInits = true;
    srv.sendConfig();
    fx.round();
    REQUIRE(fx.net().dhMode() == mp::DhMode::Server);
    CHECK_EQ(fx.net().modeReason(), "session config received");
    CHECK(fx.net().isReady());

    // first attempt: not keyed yet -> deferred, a LevelInit request goes out and is answered
    CHECK(fx.world->getOrLoadLevel(fx.level) == nullptr);
    fx.round();
    CHECK_EQ(srv.requestInits, 1);
    REQUIRE_EQ(fx.initKeys.size(), 1u);
    CHECK_EQ(fx.initKeys[0], "srv_key");
    CHECK(fx.level.keyed);
    // keyed: loads right away, well inside the 1 s delay
    DhClientLevel* lvl = fx.world->getOrLoadLevel(fx.level);
    REQUIRE(lvl != nullptr);
    CHECK(fx.net().dhMode() == mp::DhMode::Server);
}

TEST_CASE("client modes: a late reply after the fallback switches to server mode, the loaded level stays") {
    ModeFx fx;
    fx.rounds(mp::ClientNetworkState::kServerReplyTimeoutMs + 500);
    REQUIRE(fx.net().dhMode() == mp::DhMode::ClientOnly);
    DhClientLevel* lvl = fx.world->getOrLoadLevel(fx.level);
    REQUIRE(lvl != nullptr);
    CHECK_FALSE(fx.net().isReady());

    StubServer srv(fx.link.server());
    srv.sendLevelInit("minecraft:overworld", "late_key");
    srv.sendConfig();
    fx.round();
    CHECK(fx.net().dhMode() == mp::DhMode::Server);
    CHECK_EQ(fx.net().modeReason(), "level init received"); // the first of the two late frames
    CHECK(fx.net().isReady());
    REQUIRE_EQ(fx.initKeys.size(), 1u);
    CHECK_EQ(fx.initKeys[0], "late_key"); // the driver re-keys the level on this callback (ClientWorldDriver)
    // the core keeps the existing level: unload + reload under the new identifier is the driver's job
    CHECK(fx.world->getLevel(fx.level) == lvl);
}
