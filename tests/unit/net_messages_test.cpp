#include <doctest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dh/net/MessageRegistry.h"
#include "dh/net/Messages.h"
#include "dh/net/SessionConfig.h"

using namespace dh;
using namespace dh::net;

namespace {

std::vector<std::byte> fromHex(const std::string& hex) {
    std::vector<std::byte> out;
    int                    hi = -1;
    for (char c : hex) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue; // spaces
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<std::byte>(hi * 16 + v));
            hi = -1;
        }
    }
    return out;
}

std::string toHex(const std::vector<std::byte>& b) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    for (std::byte x : b) {
        s += d[static_cast<unsigned>(x) >> 4];
        s += d[static_cast<unsigned>(x) & 15];
    }
    return s;
}

template <class T>
std::unique_ptr<T> roundTrip(const T& m) {
    auto frame = encodeFrame(m);
    auto res   = decodeFrame(frame);
    REQUIRE(res.has_value());
    std::unique_ptr<Message> base = std::move(res).value();
    REQUIRE(base->id() == m.id());
    T* typed = dynamic_cast<T*>(base.get());
    REQUIRE(typed != nullptr);
    base.release();
    // byte-stable: re-encoding the decoded message gives the same frame
    CHECK(encodeFrame(*typed) == frame);
    return std::unique_ptr<T>(typed);
}

std::string frameHex(const Message& m) { return toHex(encodeFrame(m)); }

} // namespace

TEST_CASE("net: protocol version and ids") {
    CHECK(kProtocolVersion == 0x4010);
    CHECK(MsgIds::CloseReason == 1);
    CHECK(MsgIds::FullDataSplit == 10);
    for (MsgId i = 1; i <= MsgIds::Last; i++) {
        auto m = create(i);
        REQUIRE(m);
        CHECK(m->id() == i);
    }
    CHECK(create(0) == nullptr);
    CHECK(create(11) == nullptr); // CodecCrash (dev only) is not ported
}

TEST_CASE("net: round trips") {
    SUBCASE("CloseReason") {
        CloseReasonMessage m;
        m.reason = "bye \xC3\xA4";
        CHECK(roundTrip(m)->reason == m.reason);
        CloseReasonMessage e;
        CHECK(roundTrip(e)->reason.empty());
    }
    SUBCASE("LevelInit") {
        LevelInitMessage m;
        m.dimensionResourceLocation = "minecraft:overworld";
        m.serverKey                 = "srv-1_x";
        m.levelKey                  = "pre@ns:path";
        m.serverTime                = -123456789012345LL;
        auto d                      = roundTrip(m);
        CHECK(d->levelKey == m.levelKey);
        CHECK(d->serverTime == m.serverTime);
        m.serverKey.clear(); // empty server key allowed
        CHECK(roundTrip(m)->serverKey.empty());
    }
    SUBCASE("RequestLevelInit") {
        RequestLevelInitMessage m;
        m.dimensionResourceLocation = "minecraft:the_nether";
        CHECK(roundTrip(m)->dimensionResourceLocation == m.dimensionResourceLocation);
    }
    SUBCASE("SessionConfig") {
        SessionConfigMessage m;
        m.config.generatorPlan          = GeneratorPlan::ChunksOnly;
        m.config.generationCenterChunkX = -77;
        m.config.enableRealTimeUpdates  = false;
        m.config.playerBandwidthLimit   = 0;
        CHECK(roundTrip(m)->config == m.config);
    }
    SUBCASE("Cancel") {
        CancelMessage m;
        m.futureId = FutureId::make(9, true, 42);
        auto d     = roundTrip(m);
        CHECK(FutureId::wire(d->futureId) == FutureId::wire(m.futureId)); // session id is not on the wire
        CHECK((d->futureId >> 32) == 0);
    }
    SUBCASE("Exception") {
        ExceptionMessage m;
        m.futureId = 7;
        m.kind     = RemoteException::SectionRequiresSplitting;
        m.message  = "too big";
        auto d     = roundTrip(m);
        CHECK(d->kind == m.kind);
        CHECK(d->message == m.message);
    }
    SUBCASE("FullDataSourceRequest optional present/absent") {
        FullDataSourceRequestMessage m;
        m.futureId   = 1;
        m.levelName  = "lvl";
        m.sectionPos = -0x123456789ALL;
        auto a       = roundTrip(m);
        CHECK(!a->clientTimestamp.has_value());
        CHECK(a->sectionPos == m.sectionPos);
        m.clientTimestamp = 1700000000000LL;
        auto b            = roundTrip(m);
        REQUIRE(b->clientTimestamp.has_value());
        CHECK(*b->clientTimestamp == 1700000000000LL);
    }
    SUBCASE("FullDataSourceResponse optional + empty/non-empty list") {
        FullDataSourceResponseMessage m;
        CHECK(!roundTrip(m)->payload.has_value());
        m.payload.emplace();
        m.payload->dtoBufferId = 5;
        auto empty             = roundTrip(m);
        REQUIRE(empty->payload.has_value());
        CHECK(empty->payload->beaconBeams.empty());
        m.payload->beaconBeams = {
            {1,  2,  3,  static_cast<std::int32_t>(0xFF112233)},
            {-1, -2, -3, 0                                    }
        };
        auto full = roundTrip(m);
        REQUIRE(full->payload.has_value());
        CHECK(*full->payload == *m.payload);
    }
    SUBCASE("FullDataPartialUpdate") {
        FullDataPartialUpdateMessage m;
        m.levelName           = "lvl";
        m.payload.dtoBufferId = 11;
        m.payload.beaconBeams = {
            {4, 5, 6, 7}
        };
        auto d = roundTrip(m);
        CHECK(d->levelName == "lvl");
        CHECK(d->payload == m.payload);
    }
    SUBCASE("FullDataSplit") {
        FullDataSplitMessage m;
        m.bufferId = -3;
        m.isFirst  = true;
        CHECK(roundTrip(m)->buffer.empty());
        m.buffer = {std::byte{1}, std::byte{0}, std::byte{0xFF}};
        auto d   = roundTrip(m);
        CHECK(d->buffer == m.buffer);
        CHECK(d->bufferId == -3);
        CHECK(d->isFirst);
    }
}

TEST_CASE("net: hand-computed Java byte vectors") {
    SUBCASE("CloseReason") {
        CloseReasonMessage m;
        m.reason = "hi";
        // version 4010, id 0001, u16 len 0002, 'h' 'i'
        CHECK(
            frameHex(m)
            == "401000010002"
               "6869"
        );
    }
    SUBCASE("FullDataSourceRequest without / with timestamp") {
        FullDataSourceRequestMessage m;
        m.futureId   = FutureId::make(0xABCD, true, 1); // wire = 0x80000001
        m.levelName  = "a";
        m.sectionPos = 0x0102030405060708LL;
        CHECK(
            frameHex(m)
            == "4010"
               "0007"
               "80000001"
               "0001"
               "61"
               "0102030405060708"
               "00"
        );
        m.clientTimestamp = 5;
        CHECK(
            frameHex(m)
            == "4010"
               "0007"
               "80000001"
               "0001"
               "61"
               "0102030405060708"
               "01"
               "0000000000000005"
        );
    }
    SUBCASE("FullDataSplit") {
        FullDataSplitMessage m;
        m.bufferId = 3;
        m.buffer   = {std::byte{0xAA}, std::byte{0xBB}};
        m.isFirst  = true;
        CHECK(
            frameHex(m)
            == "4010"
               "000a"
               "00000003"
               "00000002"
               "aabb"
               "01"
        );
    }
    SUBCASE("SessionConfig defaults") {
        SessionConfigMessage m;
        // plan 0 | 4096 | 0 | 0 | 0 | 20 | true | 256 | true | 4096 | 50 | 500
        CHECK(
            frameHex(m)
            == "4010"
               "0004"
               "00"
               "00001000"
               "00000000"
               "00000000"
               "00000000"
               "00000014"
               "01"
               "00000100"
               "01"
               "00001000"
               "00000032"
               "000001f4"
        );
    }
    SUBCASE("Exception") {
        ExceptionMessage m;
        m.futureId = 2;
        m.kind     = RemoteException::RequestOutOfRange;
        m.message  = "abc";
        CHECK(
            frameHex(m)
            == "4010"
               "0006"
               "00000002"
               "00000001"
               "0003"
               "616263"
        );
    }
    SUBCASE("FullDataSourceResponse with one beacon") {
        FullDataSourceResponseMessage m;
        m.futureId = 0;
        m.payload.emplace();
        m.payload->dtoBufferId = 1;
        m.payload->beaconBeams = {
            {1, 2, 3, -1}
        };
        CHECK(
            frameHex(m)
            == "4010"
               "0008"
               "00000000"
               "01"
               "00000001"
               "00000001"
               "00000001"
               "00000002"
               "00000003"
               "ffffffff"
        );
    }
}

TEST_CASE("net: frame rejections") {
    CloseReasonMessage m;
    m.reason   = "hi";
    auto frame = encodeFrame(m);

    SUBCASE("wrong version (Java 16)") {
        auto bad = frame;
        bad[0]   = std::byte{0};
        bad[1]   = std::byte{16};
        auto r   = decodeFrame(bad);
        REQUIRE(!r.has_value());
        CHECK(r.error().code == ErrorCode::Unsupported);
    }
    SUBCASE("unknown id") {
        auto bad = frame;
        bad[3]   = std::byte{99};
        auto r   = decodeFrame(bad);
        REQUIRE(!r.has_value());
        CHECK(r.error().code == ErrorCode::Unsupported);
        bad[3] = std::byte{0};
        CHECK(!decodeFrame(bad).has_value());
    }
    SUBCASE("truncated at every length") {
        for (std::size_t n = 0; n < frame.size(); n++) {
            auto r = decodeFrame(std::span<const std::byte>(frame.data(), n));
            CHECK(!r.has_value());
        }
        CHECK(decodeFrame(frame).has_value());
    }
    SUBCASE("trailing garbage") {
        auto bad = frame;
        bad.push_back(std::byte{0});
        auto r = decodeFrame(bad);
        REQUIRE(!r.has_value());
        CHECK(r.error().code == ErrorCode::Corrupt);
    }
    SUBCASE("huge declared lengths do not allocate or crash") {
        CHECK(!decodeFrame(fromHex("4010 000a 00000001 7fffffff aa")).has_value());          // split size
        CHECK(!decodeFrame(fromHex("4010 000a 00000001 ffffffff aa")).has_value());          // negative size
        CHECK(!decodeFrame(fromHex("4010 0008 00000000 01 00000001 7fffffff")).has_value()); // beacon count
        CHECK(!decodeFrame(fromHex("4010 0006 00000000 00000009 0000")).has_value());        // exception index
        CHECK(!decodeFrame(fromHex("4010 0004 07 00000000")).has_value());                   // plan ordinal
    }
}

TEST_CASE("net: LevelInit validation") {
    auto frameFor = [](const std::string& dim, const std::string& server, const std::string& level) {
        LevelInitMessage m;
        m.dimensionResourceLocation = dim;
        m.serverKey                 = server;
        m.levelKey                  = level;
        return decodeFrame(encodeFrame(m)).has_value();
    };
    CHECK(frameFor("minecraft:overworld", "abc", "a-b_C9"));
    CHECK(frameFor("x", "", "p@n:path"));
    CHECK(frameFor("x", "k", std::string(150, 'a')));
    CHECK(!frameFor("x", "k", std::string(151, 'a')));
    CHECK(!frameFor("x", "k", ""));
    CHECK(!frameFor("x", "k", "bad name"));
    CHECK(!frameFor("x", "k", "a/b"));
    CHECK(!frameFor("x", "k", "@n"));
    CHECK(!frameFor("x", "k", "p@"));
    CHECK(!frameFor("x", "k", "n:"));
    CHECK(!frameFor("x", "k", "a@b@c"));
    CHECK(!frameFor("x", "k", "a:b:c"));
    CHECK(!frameFor("x", "bad key", "ok"));
    CHECK(!frameFor("x", std::string(151, 'k'), "ok"));
    CHECK(!frameFor(std::string(151, 'd'), "k", "ok"));
    CHECK(isValidServerKey("a_b-c"));
    CHECK(!isValidServerKey(""));
}

TEST_CASE("net: future id composition") {
    CHECK(FutureId::make(0, false, 0) == 0);
    CHECK(FutureId::make(1, false, 0) == 0x100000000ull);
    CHECK(FutureId::make(0, true, 0) == 0x80000000ull); // no sign extension into the session bits
    CHECK(FutureId::make(0xFFFFFFFFu, true, 0x7FFFFFFF) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(FutureId::make(0, false, 0x80000001u) == 1); // counter limited to 31 bits
    CHECK(FutureId::make(5, true, 3) == ((5ull << 32) | 0x80000003ull));

    CancelMessage m;
    m.futureId = FutureId::make(0, true, 9);
    m.setSession(0x12345678);
    CHECK(m.futureId == ((0x12345678ull << 32) | 0x80000009ull));

    // decoding keeps the local session id, replaces only the wire bits
    auto frame = encodeFrame(m);
    CHECK(
        toHex(frame)
        == "4010"
           "0005"
           "80000009"
    );
    CancelMessage target;
    target.setSession(0x77);
    ByteReader r(std::span<const std::byte>(frame).subspan(4));
    REQUIRE(target.decode(r));
    CHECK(target.futureId == ((0x77ull << 32) | 0x80000009ull));
}

TEST_CASE("net: SessionConfig constrain") {
    SessionConfigValues c, s;

    SUBCASE("min fields") {
        c.maxGenerationRequestDistance = 1000;
        s.maxGenerationRequestDistance = 2000;
        c.generationRequestRateLimit   = 30;
        s.generationRequestRateLimit   = 10;
        c.realTimeUpdateDistanceRadius = 64;
        s.realTimeUpdateDistanceRadius = 512;
        c.maxSyncOnLoadRequestDistance = 5000;
        s.maxSyncOnLoadRequestDistance = 300;
        c.syncOnLoadRateLimit          = 7;
        s.syncOnLoadRateLimit          = 70;
        auto o                         = constrain(c, s);
        CHECK(o.maxGenerationRequestDistance == 1000);
        CHECK(o.generationRequestRateLimit == 10);
        CHECK(o.realTimeUpdateDistanceRadius == 64);
        CHECK(o.maxSyncOnLoadRequestDistance == 300);
        CHECK(o.syncOnLoadRateLimit == 7);
    }
    SUBCASE("booleans use and") {
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++) {
                c.enableRealTimeUpdates = a;
                s.enableRealTimeUpdates = b;
                c.synchronizeOnLoad     = b;
                s.synchronizeOnLoad     = a;
                auto o                  = constrain(c, s);
                CHECK(o.enableRealTimeUpdates == (a && b));
                CHECK(o.synchronizeOnLoad == (a && b));
            }
    }
    SUBCASE("server overrides generation bounds") {
        c.generationCenterChunkX   = 1;
        s.generationCenterChunkX   = -5;
        c.generationCenterChunkZ   = 2;
        s.generationCenterChunkZ   = 6;
        c.generationMaxChunkRadius = 99;
        s.generationMaxChunkRadius = 0;
        auto o                     = constrain(c, s);
        CHECK(o.generationCenterChunkX == -5);
        CHECK(o.generationCenterChunkZ == 6);
        CHECK(o.generationMaxChunkRadius == 0);
    }
    SUBCASE("bandwidth: 0 means unlimited") {
        auto bw = [&](int cv, int sv) {
            c.playerBandwidthLimit = cv;
            s.playerBandwidthLimit = sv;
            return constrain(c, s).playerBandwidthLimit;
        };
        CHECK(bw(0, 0) == 0);
        CHECK(bw(0, 300) == 300);
        CHECK(bw(300, 0) == 300);
        CHECK(bw(100, 300) == 100);
        CHECK(bw(300, 100) == 100);
    }
    SUBCASE("generator plan") {
        auto plan = [&](GeneratorPlan cp, GeneratorPlan sp) {
            c.generatorPlan = cp;
            s.generatorPlan = sp;
            return constrain(c, s).generatorPlan;
        };
        using P = GeneratorPlan;
        CHECK(plan(P::Disabled, P::SurfaceThenChunks) == P::Disabled);
        CHECK(plan(P::SurfaceThenChunks, P::Disabled) == P::Disabled);
        CHECK(plan(P::SurfaceThenChunks, P::SurfaceThenChunks) == P::SurfaceThenChunks);
        CHECK(plan(P::SurfaceThenChunks, P::SurfaceOnly) == P::SurfaceOnly);
        CHECK(plan(P::SurfaceOnly, P::SurfaceThenChunks) == P::SurfaceOnly);
        CHECK(plan(P::ChunksOnly, P::SurfaceThenChunks) == P::ChunksOnly);
        CHECK(plan(P::ChunksOnly, P::ChunksOnly) == P::ChunksOnly);
        // no common generator: server plan wins
        CHECK(plan(P::SurfaceOnly, P::ChunksOnly) == P::ChunksOnly);
        CHECK(plan(P::ChunksOnly, P::SurfaceOnly) == P::SurfaceOnly);
    }
    SUBCASE("defaults constrain to themselves") { CHECK(constrain(c, s) == c); }
}

TEST_CASE("net: decoder fuzz") {
    std::mt19937 rng(0xD15EA5E);
    int          decoded = 0;
    for (int iter = 0; iter < 10000; iter++) {
        const std::size_t      len = rng() % 80;
        std::vector<std::byte> buf(len);
        for (auto& b : buf) b = static_cast<std::byte>(rng() & 0xFF);
        if (len >= 4 && (iter & 1)) { // reach the message bodies half the time
            buf[0] = std::byte{0x40};
            buf[1] = std::byte{0x10};
            buf[2] = std::byte{0};
            buf[3] = static_cast<std::byte>(1 + rng() % 10);
        }
        auto r = decodeFrame(buf);
        if (r.has_value()) {
            decoded++;
            (void)encodeFrame(*r.value());
        }
    }
    CHECK(decoded > 0);
}
