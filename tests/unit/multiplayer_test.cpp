#include <doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/gen/GeneratedProvider.h"
#include "dh/multiplayer/ClientNetworkState.h"
#include "dh/multiplayer/ClientRequestQueue.h"
#include "dh/multiplayer/Payload.h"
#include "dh/multiplayer/ServerRequestHandler.h"
#include "dh/net/MessageRegistry.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/ITransport.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/Codec.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/thread/ManualExecutor.h"

using namespace dh;
using namespace dh::mp;

namespace {

/// Records every decoded message per session.
struct Collector final : ITransportListener {
    struct Item {
        SessionId                     session;
        std::unique_ptr<net::Message> msg;
    };
    std::vector<Item>      items;
    std::vector<SessionId> connected;

    void onConnected(SessionId s, std::string_view) override { connected.push_back(s); }
    void onFrame(SessionId s, std::span<const std::byte> f) override {
        auto d = net::decodeFrame(f);
        REQUIRE(d.has_value());
        items.push_back({s, std::move(*d)});
    }
    void onDisconnected(SessionId) override {}

    template <class T>
    std::vector<T*> all(net::MsgId id, SessionId s = 0) {
        std::vector<T*> out;
        for (auto& i : items)
            if (i.msg->id() == id && (s == 0 || i.session == s)) out.push_back(static_cast<T*>(i.msg.get()));
        return out;
    }
};

struct Registry {
    MapBlockRegistry reg{false};
    Registry() {
        for (int i = 0; i < 4; ++i) {
            BlockInfo b;
            b.serial = "minecraft:block_" + std::to_string(i);
            reg.addBlock(std::move(b));
        }
        for (int i = 0; i < 2; ++i) {
            BiomeInfo b;
            b.serial = "minecraft:biome_" + std::to_string(i);
            reg.addBiome(std::move(b));
        }
    }
};

Registry& registry() {
    static Registry r;
    return r;
}

std::uint64_t secPos(int x, int z) { return sec::encode(6, x, z); }

FullDataSource makeSource(std::uint64_t pos, int seed, std::int64_t modified, std::int8_t step = 8) {
    FullDataSource s = FullDataSource::createEmpty(pos);
    for (int i = 0; i < 4; ++i)
        s.mapping.addIfAbsent(
            registry().reg.resolveBiome("minecraft:biome_" + std::to_string(i % 2)),
            registry().reg.resolveBlock("minecraft:block_" + std::to_string(i))
        );
    for (int x = 0; x < 64; ++x)
        for (int z = 0; z < 64; ++z) {
            const int top  = 100 + ((x * 7 + z * 3 + seed) % 40);
            s.column(x, z) = {
                fdp::encode((x + z + seed) % 4, 20, top - 20, 0, 15),
                fdp::encode((x + seed) % 4, 30, top - 50, 0, 15)
            };
            s.columnGenerationStep[static_cast<std::size_t>(FullDataSource::relativePosToIndex(x, z))] = step;
        }
    s.isEmpty            = false;
    s.createdUnixMs      = modified;
    s.lastModifiedUnixMs = modified;
    return s;
}

void storeSource(MemoryLodStore& store, const FullDataSource& s) {
    auto rec = encode(s);
    REQUIRE(rec.has_value());
    REQUIRE(store.put(*rec).has_value());
}

std::shared_ptr<FullDataPayload> rawPayload(std::size_t n, std::int32_t id) {
    auto p      = std::make_shared<FullDataPayload>();
    p->bufferId = id;
    p->bytes.resize(n);
    for (std::size_t i = 0; i < n; ++i) p->bytes[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    return p;
}

//----------------------------------------------------------------------------

struct SenderFx {
    LoopbackLink     link{32 * 1024};
    Collector        client;
    BandwidthLimiter shared;
    SessionId        sid;
    explicit SenderFx(int global = 0) : shared([global] { return global; }) {
        link.client().setListener(&client);
        sid = link.connect();
        link.pump();
    }
};

} // namespace

TEST_CASE("payload: split frame overhead constant matches the K1 codec") {
    net::FullDataSplitMessage m;
    m.bufferId = 5;
    m.buffer.assign(100, std::byte{1});
    CHECK(net::encodeFrame(m).size() == 100 + kSplitFrameOverhead);
}

TEST_CASE("sender: 5 MB at 200 KB/s completes in ~500 ticks, final message after the last split") {
    SenderFx      fx;
    PayloadSender sender(fx.link.server(), fx.sid, [] { return 200; }, fx.shared);
    CHECK(sender.chunkSize() == 32 * 1024 - kSplitFrameOverhead);
    CHECK(PayloadSender::bytesPerTick(200) == 10001);

    auto payload = rawPayload(5'000'000, 42);
    bool done    = false;
    sender.sendInChunks(payload, [&] {
        done = true;
        net::CloseReasonMessage marker;
        marker.reason = "done";
        fx.link.server().send(fx.sid, net::encodeFrame(marker));
    });
    CHECK(fx.shared.activeSenders() == 1);

    PayloadReceiver receiver(*std::make_unique<ManualClock>());
    ManualClock     clock;
    PayloadReceiver rx(clock);
    int             ticks = 0;
    while (!done && ticks < 2000) {
        sender.tick();
        ++ticks;
        fx.link.pump();
    }
    fx.link.pumpAll();
    CHECK(std::abs(ticks - 500) <= 1);
    CHECK(sender.failedTransfers() == 0);
    CHECK(fx.shared.activeSenders() == 0);

    REQUIRE(!fx.client.items.empty());
    CHECK(fx.client.items.back().msg->id() == net::MsgIds::CloseReason); // marker is last
    for (auto& i : fx.client.items) {
        if (i.msg->id() == net::MsgIds::FullDataSplit)
            rx.receiveChunk(*static_cast<net::FullDataSplitMessage*>(i.msg.get()));
    }
    auto bytes = rx.take(42);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == payload->bytes);
}

TEST_CASE("sender: 0 = unlimited sends everything in one tick") {
    SenderFx      fx;
    PayloadSender sender(fx.link.server(), fx.sid, [] { return 0; }, fx.shared);
    bool          done = false;
    sender.sendInChunks(rawPayload(3'000'000, 1), [&] { done = true; });
    sender.tick();
    CHECK(done);
}

TEST_CASE("sender: effective rate treats 0 as unlimited on either side") {
    CHECK(PayloadSender::effectiveRate(0, 200) == 200);
    CHECK(PayloadSender::effectiveRate(500, 200) == 200);
    CHECK(PayloadSender::effectiveRate(100, 200) == 100);
    CHECK(PayloadSender::effectiveRate(100, 0) == 100);
    CHECK(PayloadSender::effectiveRate(0, 0) == 0);
}

TEST_CASE("sender: two senders share the global limit") {
    auto run = [](int senders) {
        LoopbackLink link(32 * 1024);
        Collector    client;
        link.client().setListener(&client);
        BandwidthLimiter                            shared([] { return 400; });
        std::vector<SessionId>                      sids;
        std::vector<std::unique_ptr<PayloadSender>> ss;
        std::vector<bool>                           done(static_cast<std::size_t>(senders), false);
        for (int i = 0; i < senders; ++i) {
            sids.push_back(link.connect());
            ss.push_back(std::make_unique<PayloadSender>(link.server(), sids.back(), [] { return 0; }, shared));
        }
        link.pump();
        for (int i = 0; i < senders; ++i)
            ss[static_cast<std::size_t>(i)]->sendInChunks(rawPayload(1'000'000, i), [&done, i] {
                done[static_cast<std::size_t>(i)] = true;
            });
        CHECK(shared.activeSenders() == static_cast<std::size_t>(senders));
        CHECK(shared.bandwidthShare() == 400 / senders);
        int ticks = 0;
        while (ticks < 1000 && std::find(done.begin(), done.end(), false) != done.end()) {
            for (auto& s : ss) s->tick();
            ++ticks;
            link.pump();
        }
        return ticks;
    };
    const int solo   = run(1);
    const int shared = run(2);
    CHECK(std::abs(solo - 50) <= 1);
    CHECK(std::abs(shared - 100) <= 1);
}

TEST_CASE("sender: transport refusal drops the transfer and runs onFailure") {
    LoopbackLink     link(64);
    BandwidthLimiter shared([] { return 0; });
    SessionId        sid = link.connect();
    PayloadSender    sender(link.server(), sid, [] { return 0; }, shared);
    link.disconnect(sid);
    bool failed = false, done = false;
    sender.sendInChunks(rawPayload(500, 1), [&] { done = true; }, [&] { failed = true; });
    sender.tick();
    CHECK(failed);
    CHECK_FALSE(done);
    CHECK(sender.pendingTransfers() == 0);
}

TEST_CASE("receiver: out-of-order, duplicate and stale chunks") {
    ManualClock     clock;
    PayloadReceiver rx(clock);

    FullDataSource src = makeSource(secPos(1, 2), 3, 1000);
    auto           pl  = makePayload(src, {}, 9);
    REQUIRE(pl.has_value());
    const auto&       bytes = pl->bytes;
    const std::size_t third = bytes.size() / 3;

    auto chunk = [&](std::size_t from, std::size_t to, bool first) {
        net::FullDataSplitMessage m;
        m.bufferId = 9;
        m.isFirst  = first;
        m.buffer.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(from),
            bytes.begin() + static_cast<std::ptrdiff_t>(to)
        );
        return m;
    };

    // non-first chunk without a start is dropped
    rx.receiveChunk(chunk(third, 2 * third, false));
    CHECK(rx.bufferCount() == 0);
    CHECK(rx.droppedChunks() == 1);

    // duplicate first chunk restarts the buffer; the stream then completes correctly
    rx.receiveChunk(chunk(0, third, true));
    rx.receiveChunk(chunk(0, third, true));
    rx.receiveChunk(chunk(third, 2 * third, false));
    rx.receiveChunk(chunk(2 * third, bytes.size(), false));
    auto decoded = rx.decode(9, registry().reg);
    REQUIRE(decoded.has_value());
    CHECK(decoded->contentEquals(*dh::decode(*encode(src), registry().reg)));
    CHECK(rx.bufferCount() == 0); // removed after decode

    // a duplicated middle chunk corrupts the stream: decode fails cleanly and the buffer is gone
    rx.receiveChunk(chunk(0, third, true));
    rx.receiveChunk(chunk(third, 2 * third, false));
    rx.receiveChunk(chunk(third, 2 * third, false));
    rx.receiveChunk(chunk(2 * third, bytes.size(), false));
    CHECK_FALSE(rx.decode(9, registry().reg).has_value());
    CHECK(rx.bufferCount() == 0);

    // missing chunk -> truncated
    rx.receiveChunk(chunk(0, third, true));
    CHECK_FALSE(rx.decode(9, registry().reg).has_value());
}

TEST_CASE("receiver: stale buffers expire, access refreshes") {
    ManualClock               clock;
    PayloadReceiver           rx(clock);
    net::FullDataSplitMessage m;
    m.bufferId = 1;
    m.isFirst  = true;
    m.buffer.assign(10, std::byte{1});
    rx.receiveChunk(m);
    clock.advanceMillis(20'000);
    m.isFirst = false;
    rx.receiveChunk(m); // refresh
    clock.advanceMillis(20'000);
    CHECK(rx.has(1));
    clock.advanceMillis(31'000);
    CHECK(rx.expire() == 1);
    CHECK_FALSE(rx.take(1).has_value());
}

TEST_CASE("congestion control: additive increase, decrease on shortfall") {
    ManualClock             clock;
    int                     updates = 0;
    ClientCongestionControl cc(clock, [&] { ++updates; });
    CHECK(cc.getDesiredRate() == 50);
    cc.onPayloadReceived(60'000);
    clock.advanceMillis(1000);
    cc.onPayloadReceived(10); // interval elapsed: 60000 >= 50000 -> 100000
    CHECK(cc.getDesiredRate() == 100);
    CHECK(updates == 1);
    clock.advanceMillis(1000);
    cc.onPayloadReceived(1); // throughput 10 < 100000 -> max(10 - 25000, 1000)
    CHECK(cc.getDesiredRate() == 1);
    CHECK(updates == 2);
}

//==============================================================================
// server
//==============================================================================

namespace {

struct FakeRetrievalQueue final : IRetrievalQueue {
    std::vector<std::uint64_t>                    submitted;
    std::vector<std::uint64_t>                    removedPositions;
    std::vector<std::shared_ptr<RetrievalFuture>> futures;
    std::shared_ptr<RetrievalFuture>              submit(std::uint64_t pos, std::uint8_t) override {
        submitted.push_back(pos);
        futures.push_back(std::make_shared<RetrievalFuture>());
        return futures.back();
    }
    void startAndSetTargetPos(BlockPos2D) override {}
    void removeRetrievalRequestIf(std::function<bool(std::uint64_t)> f) override {
        for (std::size_t i = 0; i < submitted.size(); ++i)
            if (f(submitted[i])) {
                removedPositions.push_back(submitted[i]);
                futures[i]->cancel();
            }
    }
    std::size_t waitingTaskCount() const override { return 0; }
    std::size_t inProgressTaskCount() const override { return 0; }
    void        shutdown(bool) override {}
};

struct ServerFx {
    LoopbackLink         link;
    ManualClock          clock;
    ManualExecutor       fileExec, work;
    MemoryLodStore       store;
    GeneratedProvider    provider;
    ServerPlayers        players;
    ServerNetwork        network;
    ServerRequestHandler handler;
    Collector            raw;
    FakeRetrievalQueue   queue;

    explicit ServerFx(std::size_t mtu = 4096)
    : link(mtu),
      provider(store, registry().reg, clock, fileExec),
      players(link.server(), clock),
      network(players),
      handler(players, provider, work, LevelInfo{"lvl", "minecraft:overworld", "key1"}) {
        network.addLevel(handler);
        link.client().setListener(&raw);
        provider.setRetrievalQueue(&queue);
    }

    SessionId connect(bool sendConfig = true, BlockPos2D pos = {0, 0}) {
        SessionId sid = link.connect();
        link.pumpAll();
        players.setPosition(sid, "lvl", pos);
        if (sendConfig) {
            net::SessionConfigMessage m;
            link.client().send(sid, net::encodeFrame(m));
            link.pumpAll();
        }
        return sid;
    }

    void request(SessionId sid, std::uint32_t wire, std::uint64_t pos, std::optional<std::int64_t> ts = std::nullopt) {
        net::FullDataSourceRequestMessage m;
        m.futureId        = wire;
        m.levelName       = "lvl";
        m.sectionPos      = static_cast<std::int64_t>(pos);
        m.clientTimestamp = ts;
        link.client().send(sid, net::encodeFrame(m));
    }
    void cancel(SessionId sid, std::uint32_t wire) {
        net::CancelMessage m;
        m.futureId = wire;
        link.client().send(sid, net::encodeFrame(m));
    }

    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            link.pumpAll();
            fileExec.runAll();
            work.runAll();
            network.tick();
            fileExec.runAll();
            work.runAll();
            clock.advanceMillis(50);
            link.pumpAll();
        }
    }

    std::vector<net::ExceptionMessage*> exceptions(SessionId s = 0) {
        return raw.all<net::ExceptionMessage>(net::MsgIds::Exception, s);
    }
};

} // namespace

TEST_CASE("server: grouped generation requests are encoded once and served to every player") {
    ServerFx   fx;
    const auto pos = secPos(2, 3);
    storeSource(fx.store, makeSource(pos, 1, 5000));
    SessionId a = fx.connect(), b = fx.connect();

    fx.request(a, 1, pos);
    fx.request(b, 1, pos);
    fx.step(); // requests arrive, group created, source loaded
    CHECK(fx.handler.activeTickets() == 2);
    CHECK(fx.handler.groupCount() <= 1);
    fx.step(3);

    auto resp = fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse);
    REQUIRE(resp.size() == 2);
    CHECK(resp[0]->payload->dtoBufferId == resp[1]->payload->dtoBufferId);
    CHECK(fx.handler.stats().payloadsEncoded == 1);
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.queue.submitted.empty());
}

TEST_CASE("server: ungenerated section is queued once, served after generation, tickets released") {
    ServerFx   fx;
    const auto pos = secPos(0, 0);
    SessionId  a = fx.connect(), b = fx.connect();
    fx.request(a, 1, pos);
    fx.request(b, 2, pos);
    fx.step(2);
    CHECK(fx.queue.submitted.size() == 1);
    CHECK(fx.handler.activeTickets() == 2);
    CHECK(fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse).empty());

    storeSource(fx.store, makeSource(pos, 2, 6000));
    fx.provider.invalidate(pos);
    fx.handler.onWorldGenTaskComplete(pos);
    fx.step(4);
    CHECK(fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse).size() == 2);
    CHECK(fx.handler.activeTickets() == 0);
}

TEST_CASE("server: cancel releases the ticket and the retrieval request") {
    ServerFx   fx;
    const auto pos = secPos(0, 0);
    SessionId  a = fx.connect(), b = fx.connect();
    fx.request(a, 1, pos);
    fx.request(b, 1, pos);
    fx.step(2);
    REQUIRE(fx.queue.submitted.size() == 1);
    CHECK(fx.handler.activeTickets() == 2);

    fx.cancel(a, 1);
    fx.step();
    CHECK(fx.handler.activeTickets() == 1);
    CHECK(fx.queue.removedPositions.empty()); // b still waits
    fx.cancel(b, 1);
    fx.step();
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.queue.removedPositions.size() == 1);
    CHECK(fx.handler.groupCount() == 0);

    // a player cannot cancel another player's request (same wire id, different session)
    fx.request(a, 7, secPos(5, 5));
    fx.step();
    fx.cancel(b, 7);
    fx.step();
    CHECK(fx.handler.activeTickets() == 1);
}

TEST_CASE("server: exception responses release on every path") {
    ServerFx  fx;
    SessionId a = fx.connect();

    // splitting: ungenerated section above the lowest data detail
    fx.request(a, 1, sec::encode(7, 0, 0));
    fx.step(2);
    auto ex = fx.exceptions();
    REQUIRE(ex.size() == 1);
    CHECK(ex[0]->kind == net::RemoteException::SectionRequiresSplitting);
    CHECK(fx.handler.activeTickets() == 0);

    // out of range
    fx.players.settings.values.maxGenerationRequestDistance = 10;
    fx.request(a, 2, secPos(100, 0));
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestOutOfRange);
    fx.players.settings.values.maxGenerationRequestDistance = 4096;

    // bounds
    fx.players.settings.values.generationMaxChunkRadius = 4;
    fx.request(a, 3, secPos(50, 50));
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestOutOfRange);
    fx.players.settings.values.generationMaxChunkRadius = 0;

    // plan without surface generation cannot serve detail > block
    fx.players.settings.values.generatorPlan = net::GeneratorPlan::ChunksOnly;
    fx.request(a, 4, sec::encode(8, 0, 0));
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::SectionRequiresSplitting);

    // generation disabled
    fx.players.settings.values.generatorPlan = net::GeneratorPlan::Disabled;
    fx.request(a, 5, secPos(0, 0));
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestRejected);
    CHECK(fx.handler.activeTickets() == 0);

    // unknown level answered by the router
    net::FullDataSourceRequestMessage m;
    m.futureId  = 9;
    m.levelName = "nope";
    fx.link.client().send(a, net::encodeFrame(m));
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestRejected);
}

TEST_CASE("server: rate limiter rejects above the concurrency cap and releases on completion") {
    ServerFx fx;
    fx.players.settings.values.generationRequestRateLimit = 2;
    SessionId a                                           = fx.connect();
    fx.clock.advanceMillis(1000);
    fx.request(a, 1, secPos(0, 0));
    fx.request(a, 2, secPos(1, 0));
    fx.request(a, 3, secPos(2, 0));
    fx.step(2);
    int limited = 0;
    for (auto* e : fx.exceptions(a))
        if (e->kind == net::RemoteException::RateLimited) ++limited;
    CHECK(limited == 1);
    CHECK(fx.handler.activeTickets() == 2);
    fx.cancel(a, 1);
    fx.cancel(a, 2);
    fx.step();
    CHECK(fx.handler.activeTickets() == 0);
}

TEST_CASE("server: player leaving releases its tickets") {
    ServerFx  fx;
    SessionId a = fx.connect();
    fx.request(a, 1, secPos(0, 0));
    fx.step(2);
    CHECK(fx.handler.activeTickets() == 1);
    fx.link.disconnect(a);
    fx.step();
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.queue.removedPositions.size() == 1);
}

TEST_CASE("server: sync-on-load answers only when the server copy is newer") {
    ServerFx   fx;
    const auto pos = secPos(1, 1);
    storeSource(fx.store, makeSource(pos, 1, 1000));
    SessionId a = fx.connect();

    fx.request(a, 1, pos, 2000);       // client newer
    fx.request(a, 2, pos, 1000);       // equal -> up to date
    fx.request(a, 3, secPos(9, 9), 0); // no server data
    fx.step(3);
    auto resp = fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse);
    REQUIRE(resp.size() == 3);
    for (auto* r : resp) CHECK_FALSE(r->payload.has_value());
    CHECK(fx.handler.activeTickets() == 0);

    fx.request(a, 4, pos, 500); // server newer
    fx.step(4);
    resp = fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse);
    REQUIRE(resp.size() == 4);
    CHECK(resp[3]->payload.has_value());
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.queue.submitted.empty()); // never goes through generation

    // disabled by config
    fx.players.settings.values.synchronizeOnLoad = false;
    fx.request(a, 5, pos, 0);
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestRejected);
    fx.players.settings.values.synchronizeOnLoad = true;

    // sync distance gate
    fx.players.settings.values.maxSyncOnLoadRequestDistance = 1;
    fx.request(a, 6, secPos(50, 50), 0);
    fx.step();
    CHECK(fx.exceptions().back()->kind == net::RemoteException::RequestOutOfRange);
}

TEST_CASE("server: sync-on-load cancel releases once") {
    ServerFx   fx;
    const auto pos = secPos(1, 1);
    storeSource(fx.store, makeSource(pos, 1, 1000));
    SessionId a = fx.connect();
    fx.request(a, 1, pos, 0);
    fx.link.pumpAll();
    CHECK(fx.handler.activeTickets() == 1); // queued on the work executor, not yet run
    fx.cancel(a, 1);
    fx.link.pumpAll();
    CHECK(fx.handler.activeTickets() == 0);
    fx.step(3);
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse).empty());
}

TEST_CASE("server: real-time updates reach only nearby players") {
    ServerFx   fx;
    const auto pos   = secPos(0, 0);
    SessionId  near  = fx.connect(true, {0, 0});
    SessionId  far   = fx.connect(true, {10'000'000, 0});
    SessionId  nocfg = fx.connect(false, {0, 0});
    (void)nocfg;

    auto src = std::make_shared<const FullDataSource>(makeSource(pos, 4, 7000));
    auto r   = fx.provider.updater().update(src);
    REQUIRE(r.has_value());
    REQUIRE(*r);
    fx.step(4);
    auto upd = fx.raw.all<net::FullDataPartialUpdateMessage>(net::MsgIds::FullDataPartialUpdate);
    REQUIRE(upd.size() == 1);
    CHECK(upd[0]->levelName == "lvl");
    CHECK(fx.raw.all<net::FullDataPartialUpdateMessage>(net::MsgIds::FullDataPartialUpdate, near).size() == 1);
    CHECK(fx.raw.all<net::FullDataPartialUpdateMessage>(net::MsgIds::FullDataPartialUpdate, far).empty());
    CHECK(fx.handler.stats().realtimeUpdatesSent == 1);
}

//==============================================================================
// client
//==============================================================================

namespace {

/// Scripted server: answers the config handshake, records requests/cancels.
struct StubServer final : ITransportListener {
    LoopbackLink&                                  link;
    net::SessionConfigValues                       cfg;
    std::vector<net::FullDataSourceRequestMessage> requests;
    std::vector<std::uint32_t>                     cancels;
    std::int64_t                                   serverTime = 0;
    net::SessionConfigValues                       lastClientConfig;
    int                                            configsReceived = 0;

    explicit StubServer(LoopbackLink& l) : link(l) { link.server().setListener(this); }
    ~StubServer() override { link.server().setListener(nullptr); }

    void onConnected(SessionId, std::string_view) override {}
    void onDisconnected(SessionId) override {}
    void onFrame(SessionId s, std::span<const std::byte> f) override {
        auto d = net::decodeFrame(f);
        REQUIRE(d.has_value());
        auto& m = **d;
        if (m.id() == net::MsgIds::SessionConfig) {
            lastClientConfig = static_cast<net::SessionConfigMessage&>(m).config;
            ++configsReceived;
            net::SessionConfigMessage r;
            r.config = cfg;
            link.server().send(s, net::encodeFrame(r));
            net::LevelInitMessage li;
            li.dimensionResourceLocation = "minecraft:overworld";
            li.levelKey                  = "k";
            li.serverTime                = serverTime;
            link.server().send(s, net::encodeFrame(li));
        } else if (m.id() == net::MsgIds::FullDataSourceRequest) {
            requests.push_back(static_cast<net::FullDataSourceRequestMessage&>(m));
            sessionOfLast = s;
        } else if (m.id() == net::MsgIds::Cancel) {
            cancels.push_back(static_cast<std::uint32_t>(static_cast<net::CancelMessage&>(m).futureId));
        }
    }
    SessionId sessionOfLast = 0;

    void raise(std::size_t reqIndex, net::RemoteException kind, const std::string& msg = "x") {
        net::ExceptionMessage e;
        e.futureId = requests[reqIndex].futureId;
        e.kind     = kind;
        e.message  = msg;
        link.server().send(sessionOfLast, net::encodeFrame(e));
    }
    void answer(std::size_t reqIndex, std::optional<net::FullDataPayload> payload) {
        net::FullDataSourceResponseMessage r;
        r.futureId = requests[reqIndex].futureId;
        r.payload  = std::move(payload);
        link.server().send(sessionOfLast, net::encodeFrame(r));
    }
};

struct ClientFx {
    LoopbackLink                        link{4096};
    ManualClock                         clock;
    ManualExecutor                      work;
    StubServer                          stub;
    ClientNetworkState                  net;
    std::unique_ptr<ClientRequestQueue> queue;

    static ClientNetConfig cfg() { return {}; }

    explicit ClientFx(const std::function<void(net::SessionConfigValues&)>& tweak = {})
    : stub(link),
      net(link.client(), clock, registry().reg, cfg()) {
        if (tweak) tweak(stub.cfg);
        link.connect();
        link.pumpAll();
        REQUIRE(net.isReady());
        queue = std::make_unique<ClientRequestQueue>(net, "lvl", work);
    }
    void run() {
        link.pumpAll();
        work.runAll();
        link.pumpAll();
    }
    void tick(BlockPos2D t = {0, 0}, int advanceMs = 1000) {
        clock.advanceMillis(advanceMs);
        queue->startAndSetTargetPos(t);
        run();
    }
};

} // namespace

TEST_CASE("client: negotiation, server clock offset, session config listener") {
    ClientFx fx([](net::SessionConfigValues& c) { c.generationRequestRateLimit = 7; });
    CHECK(fx.net.sessionConfig().generationRequestRateLimit == 7);
    CHECK(fx.net.serverSupport() == ServerSupport::Full);
    CHECK(fx.net.serverTimeOffset() == fx.stub.serverTime - fx.clock.nowMillis());
}

TEST_CASE("client: in-flight cap equals the rate limit, nearest first") {
    ClientFx fx([](net::SessionConfigValues& c) { c.generationRequestRateLimit = 3; });
    std::vector<std::shared_ptr<RetrievalFuture>> fut;
    for (int x = 5; x >= 0; --x) fut.push_back(fx.queue->submit(secPos(x, 0), 0));
    CHECK(fx.queue->submit(secPos(0, 0), 0) == fut.back()); // same pos -> same future
    CHECK(fx.queue->waitingTaskCount() == 6);
    fx.tick();
    CHECK(fx.stub.requests.size() == 3);
    CHECK(fx.queue->inProgressTaskCount() == 3);
    fx.tick();
    CHECK(fx.stub.requests.size() == 3); // cap holds
    // nearest first: x = 0, 1, 2
    CHECK(fx.stub.requests[0].sectionPos == static_cast<std::int64_t>(secPos(0, 0)));
    CHECK(fx.stub.requests[1].sectionPos == static_cast<std::int64_t>(secPos(1, 0)));
    CHECK(fx.stub.requests[2].sectionPos == static_cast<std::int64_t>(secPos(2, 0)));
    CHECK_FALSE(fx.stub.requests[0].clientTimestamp.has_value());

    fx.stub.raise(0, net::RemoteException::SectionRequiresSplitting);
    fx.run();
    REQUIRE(fut[5]->isDone());
    CHECK(fut[5]->get()->kind == RetrievalResult::Kind::Split);
    CHECK(fx.queue->inProgressTaskCount() == 2);
    fx.tick();
    CHECK(fx.stub.requests.size() == 4);
    CHECK(fx.queue->inProgressTaskCount() == 3);
}

TEST_CASE("client: distance and bounds gate cancel without sending") {
    ClientFx fx([](net::SessionConfigValues& c) {
        c.maxGenerationRequestDistance = 10; // chunks
        c.generationMaxChunkRadius     = 0;
    });
    auto     far  = fx.queue->submit(secPos(100, 0), 0);
    auto     near = fx.queue->submit(secPos(0, 0), 0);
    fx.tick();
    CHECK(far->isCancelled());
    CHECK_FALSE(near->isDone());
    REQUIRE(fx.stub.requests.size() == 1);
    CHECK(fx.stub.requests[0].sectionPos == static_cast<std::int64_t>(secPos(0, 0)));
    CHECK(fx.queue->waitingTaskCount() == 1);

    ClientFx bounded([](net::SessionConfigValues& c) {
        c.generationMaxChunkRadius = 8;
        c.generationCenterChunkX   = 1000;
        c.generationCenterChunkZ   = 1000;
    });
    auto     outside = bounded.queue->submit(secPos(0, 0), 0);
    bounded.tick();
    CHECK(outside->isCancelled());
    CHECK(bounded.stub.requests.empty());
}

TEST_CASE("client: retry cap on undecodable responses, rejected -> failed") {
    ClientFx fx;
    auto     f = fx.queue->submit(secPos(0, 0), 0);
    for (int i = 0; i < 6 && !f->isDone(); ++i) {
        fx.tick();
        if (fx.stub.requests.size() > static_cast<std::size_t>(i))
            fx.stub.answer(static_cast<std::size_t>(i), net::FullDataPayload{77, {}});
        fx.run();
    }
    REQUIRE(f->isDone());
    CHECK(f->get()->kind == RetrievalResult::Kind::Failed);
    CHECK(fx.stub.requests.size() == static_cast<std::size_t>(kMaxRetryAttempts));
    CHECK(fx.queue->failedRequests() == 1);
    CHECK(fx.queue->waitingTaskCount() == 0);

    auto g = fx.queue->submit(secPos(1, 0), 0);
    fx.tick();
    fx.stub.raise(fx.stub.requests.size() - 1, net::RemoteException::RequestRejected, "nope");
    fx.run();
    REQUIRE(g->isDone());
    CHECK(g->get()->kind == RetrievalResult::Kind::Failed);
    CHECK(g->get()->error == "nope");
}

TEST_CASE("client: rate limited and out of range responses re-queue") {
    ClientFx fx;
    auto     f = fx.queue->submit(secPos(0, 0), 0);
    fx.tick();
    REQUIRE(fx.stub.requests.size() == 1);
    fx.stub.raise(0, net::RemoteException::RateLimited);
    fx.run();
    CHECK_FALSE(f->isDone());
    fx.tick({0, 0}, 10); // still inside the penalty (acquireAll drains the bucket)
    CHECK(fx.stub.requests.size() == 1);
    fx.tick({0, 0}, 3000);
    REQUIRE(fx.stub.requests.size() == 2);
    fx.stub.raise(1, net::RemoteException::RequestOutOfRange);
    fx.run();
    fx.tick({0, 0}, 3000);
    CHECK(fx.stub.requests.size() == 3);
    CHECK_FALSE(f->isDone());
}

TEST_CASE("client: removing an in-flight request cancels it on the server") {
    ClientFx fx;
    auto     f = fx.queue->submit(secPos(0, 0), 0);
    auto     g = fx.queue->submit(secPos(9, 0), 0);
    fx.tick();
    REQUIRE(fx.stub.requests.size() == 2);
    fx.queue->removeRetrievalRequestIf([](std::uint64_t p) { return p == secPos(0, 0); });
    fx.run();
    CHECK(f->isCancelled());
    CHECK_FALSE(g->isDone());
    REQUIRE(fx.stub.cancels.size() == 1);
    CHECK(fx.stub.cancels[0] == static_cast<std::uint32_t>(fx.stub.requests[0].futureId));
    CHECK(fx.queue->inProgressTaskCount() == 1);
    // a late answer for the cancelled request is ignored
    fx.stub.answer(0, std::nullopt);
    fx.run();
    CHECK(fx.queue->inProgressTaskCount() == 1);

    fx.queue->shutdown(true);
    fx.run();
    CHECK(g->isCancelled());
    CHECK(fx.queue->inProgressTaskCount() == 0);
    CHECK(fx.queue->waitingTaskCount() == 0);
}

TEST_CASE("client: sync-on-load queue sends offset timestamps, empty answer cancels") {
    ClientFx               fx;
    SyncOnLoadRequestQueue sync(fx.net, "lvl", fx.work);
    auto                   f = sync.submitRequest(secPos(0, 0), 1000);
    fx.clock.advanceMillis(1000);
    CHECK(sync.tick({0, 0}));
    fx.run();
    REQUIRE(fx.stub.requests.size() == 1);
    CHECK(fx.stub.requests[0].clientTimestamp == std::optional<std::int64_t>(1000 + fx.net.serverTimeOffset()));
    fx.stub.answer(0, std::nullopt);
    fx.run();
    CHECK(f->isCancelled());
    CHECK(sync.waitingCount() == 0);
}

//==============================================================================
// full round trip
//==============================================================================

namespace {

net::SessionConfigValues wantedWith(int playerKBps) {
    net::SessionConfigValues w;
    w.playerBandwidthLimit = playerKBps;
    return w;
}

struct RoundTripFx {
    LoopbackLink                        link;
    ManualClock                         clock;
    ManualExecutor                      fileExec, serverWork, clientWork;
    MemoryLodStore                      store;
    GeneratedProvider                   provider;
    ServerPlayers                       players;
    ServerNetwork                       network;
    ServerRequestHandler                handler;
    FakeRetrievalQueue                  genQueue;
    ClientNetworkState                  client;
    std::unique_ptr<ClientRequestQueue> queue;
    SessionId                           sid = 0;

    RoundTripFx(std::size_t mtu, int playerKBps)
    : link(mtu),
      provider(store, registry().reg, clock, fileExec),
      players(link.server(), clock),
      network(players),
      handler(players, provider, serverWork, LevelInfo{"lvl", "minecraft:overworld", "key1"}),
      client(link.client(), clock, registry().reg, ClientNetConfig{wantedWith(playerKBps), false}) {
        network.addLevel(handler);
        provider.setRetrievalQueue(&genQueue);
        sid = link.connect();
        link.pumpAll();
        players.setPosition(sid, "lvl", {0, 0});
        queue = std::make_unique<ClientRequestQueue>(client, "lvl", clientWork);
    }

    void step() {
        clock.advanceMillis(50);
        queue->startAndSetTargetPos({0, 0});
        link.pumpAll();
        fileExec.runAll();
        serverWork.runAll();
        network.tick();
        fileExec.runAll();
        serverWork.runAll();
        link.pumpAll();
        clientWork.runAll();
    }
};

} // namespace

TEST_CASE("round trip: a requested section arrives identical to the server's") {
    RoundTripFx fx(1500, 500); // MTU 1500 forces ~1.4 KB chunks, 500 KB/s -> 25 KB per tick
    const auto  pos = secPos(3, -2);
    storeSource(fx.store, makeSource(pos, 5, 9000));
    REQUIRE(fx.client.isReady());
    CHECK(fx.client.sessionConfig().playerBandwidthLimit == 500);

    auto f     = fx.queue->submit(pos, 0);
    int  ticks = 0;
    while (!f->isDone() && ticks < 3000) {
        fx.step();
        ++ticks;
    }
    REQUIRE(f->isDone());
    auto r = *f->get();
    REQUIRE(r.kind == RetrievalResult::Kind::Success);
    REQUIRE(r.source);
    auto server = fx.provider.get(pos);
    REQUIRE(server);
    CHECK(r.source->contentEquals(*server));
    CHECK(r.pos == pos);
    CHECK(r.source->applyToParent == std::optional<bool>(true));
    CHECK(fx.handler.activeTickets() == 0);
    CHECK(fx.client.receiver().bufferCount() == 0);
    MESSAGE("round trip took " << ticks << " ticks");
}

TEST_CASE("round trip: generation request completes after the world generator finishes") {
    RoundTripFx fx(4096, 0);
    const auto  pos = secPos(1, 1);
    auto        f   = fx.queue->submit(pos, 0);
    for (int i = 0; i < 5; ++i) fx.step();
    CHECK_FALSE(f->isDone());
    CHECK(fx.genQueue.submitted.size() == 1);

    storeSource(fx.store, makeSource(pos, 6, 9500));
    fx.provider.invalidate(pos);
    fx.handler.onWorldGenTaskComplete(pos);
    for (int i = 0; i < 100 && !f->isDone(); ++i) fx.step();
    REQUIRE(f->isDone());
    REQUIRE(f->get()->kind == RetrievalResult::Kind::Success);
    CHECK(f->get()->source->contentEquals(*fx.provider.get(pos)));
    CHECK(fx.handler.activeTickets() == 0);
}

TEST_CASE("round trip: real-time update decoded on the client") {
    RoundTripFx                   fx(4096, 0);
    std::optional<FullDataSource> got;
    fx.client.setPartialUpdateHandler("lvl", [&](Result<FullDataSource> r, const std::vector<net::BeaconBeam>&) {
        REQUIRE(r.has_value());
        got = std::move(*r);
    });
    fx.step(); // make sure the session config reached the server
    const auto pos = secPos(0, 0);
    auto       src = std::make_shared<const FullDataSource>(makeSource(pos, 8, 10000));
    REQUIRE(*fx.provider.updater().update(src));
    for (int i = 0; i < 20 && !got; ++i) fx.step();
    REQUIRE(got.has_value());
    // only the incoming delta is sent, not the merged section
    CHECK(got->contentEquals(*dh::decode(*encode(*src), registry().reg)));
}

TEST_CASE("server: buffer ids are unique across levels") {
    ServerFx             fx;
    ServerRequestHandler second(fx.players, fx.provider, fx.work, LevelInfo{"lvl2", "minecraft:the_nether", "key2"});
    fx.network.addLevel(second);
    const auto pos = secPos(2, 3);
    storeSource(fx.store, makeSource(pos, 1, 5000));
    SessionId a = fx.connect();
    fx.request(a, 1, pos);
    net::FullDataSourceRequestMessage m;
    m.futureId   = 2;
    m.levelName  = "lvl2";
    m.sectionPos = static_cast<std::int64_t>(pos);
    fx.link.client().send(a, net::encodeFrame(m));
    fx.step(5);
    auto resp = fx.raw.all<net::FullDataSourceResponseMessage>(net::MsgIds::FullDataSourceResponse);
    REQUIRE(resp.size() == 2);
    CHECK(resp[0]->payload->dtoBufferId != resp[1]->payload->dtoBufferId);
    fx.network.removeLevel(second);
}

TEST_CASE("server: a duplicate wire futureId is rejected and does not leak a ticket") {
    ServerFx  fx;
    SessionId a = fx.connect();
    fx.request(a, 1, secPos(0, 0));
    fx.request(a, 1, secPos(5, 5));
    fx.step(2);
    CHECK(fx.handler.activeTickets() == 1);
    REQUIRE(fx.exceptions().size() == 1);
    CHECK(fx.exceptions()[0]->kind == net::RemoteException::RequestRejected);
}

TEST_CASE("server: broadcastConfig pushes the new constrained config") {
    ServerFx  fx;
    SessionId a = fx.connect();
    fx.step();
    const auto before = fx.raw.all<net::SessionConfigMessage>(net::MsgIds::SessionConfig, a).size();
    fx.players.settings.values.generationRequestRateLimit = 3;
    fx.network.broadcastConfig();
    fx.step();
    auto cfgs = fx.raw.all<net::SessionConfigMessage>(net::MsgIds::SessionConfig, a);
    REQUIRE(cfgs.size() == before + 1);
    CHECK(cfgs.back()->config.generationRequestRateLimit == 3);
}

TEST_CASE("client: setWanted re-sends the config; adaptive speed constrains the configured limit") {
    ClientFx fx;
    CHECK(fx.stub.configsReceived == 1);
    net::SessionConfigValues w;
    w.playerBandwidthLimit = 30;
    fx.net.setWanted(w);
    fx.link.pumpAll();
    CHECK(fx.stub.configsReceived == 2);
    CHECK(fx.stub.lastClientConfig.playerBandwidthLimit == 30);
    CHECK(fx.net.isReady()); // answered again

    fx.net.setAdaptiveTransferSpeed(true); // desired 50 KB/s, configured 30 -> 30
    fx.link.pumpAll();
    CHECK(fx.stub.lastClientConfig.playerBandwidthLimit == 30);
    w.playerBandwidthLimit = 0;
    fx.net.setWanted(w); // unlimited configured -> the desired rate
    fx.link.pumpAll();
    CHECK(fx.stub.lastClientConfig.playerBandwidthLimit == 50);
}

TEST_CASE("receiver: total buffer count is capped") {
    ManualClock     clock;
    PayloadReceiver rx(clock);
    for (int i = 0; i < 300; ++i) {
        net::FullDataSplitMessage m;
        m.bufferId = i;
        m.isFirst  = true;
        m.buffer.assign(4, std::byte{1});
        rx.receiveChunk(m);
        clock.advanceMillis(1);
    }
    CHECK(rx.bufferCount() <= kMaxReceiverBuffers);
    CHECK(rx.has(299));
    CHECK_FALSE(rx.has(0));
}

TEST_CASE("sender: bandwidth share is snapshotted per pass") {
    LoopbackLink link(32 * 1024);
    Collector    client;
    link.client().setListener(&client);
    BandwidthLimiter shared([] { return 400; });
    SessionId        s1 = link.connect(), s2 = link.connect();
    PayloadSender    a(link.server(), s1, [] { return 0; }, shared);
    PayloadSender    b(link.server(), s2, [] { return 0; }, shared);
    link.pump();
    a.sendInChunks(rawPayload(100, 1), [] {}); // finishes in the first tick
    b.sendInChunks(rawPayload(1'000'000, 2), [] {});
    const int share = shared.bandwidthShare(); // 200 with both active
    a.tick(share);
    b.tick(share); // would be 400 if re-read after a finished
    link.pump();
    std::size_t bytes = 0;
    for (auto& i : client.items)
        if (i.msg->id() == net::MsgIds::FullDataSplit
            && static_cast<net::FullDataSplitMessage*>(i.msg.get())->bufferId == 2)
            bytes += static_cast<net::FullDataSplitMessage*>(i.msg.get())->buffer.size();
    CHECK(bytes == static_cast<std::size_t>(PayloadSender::bytesPerTick(200)));
}
