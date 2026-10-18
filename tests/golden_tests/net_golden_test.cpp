// Golden vectors for the net messages and SessionConfig constrain (tests/golden/net.gv.json).
// Checked: out.id, out.body, out.frame, out.roundTrip, aConstrainedByB, bConstrainedByA, aConstrainedByB_encoded.
// SKIPPED_KEYS: codec_crash_* (message id 11 is not ported; out.throws).
// Java frames carry protocol version 0x0010, ours 0x4010: the version is substituted before decoding and the
// re-encoded frame is compared with the substituted Java frame.
#include <doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dh/net/MessageRegistry.h"
#include "dh/net/Messages.h"
#include "dh/net/SessionConfig.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::net;
using namespace dh::test;

namespace {

std::vector<std::byte> fromHex(const std::string& hex) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::byte>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string toHexStr(const std::vector<std::byte>& b) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    for (std::byte x : b) {
        s.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        s.push_back(d[std::to_integer<unsigned>(x) & 15]);
    }
    return s;
}

SessionConfigValues fromArray(const nlohmann::json& a) {
    SessionConfigValues v;
    v.generatorPlan                = static_cast<GeneratorPlan>(a.at(0).get<int>());
    v.maxGenerationRequestDistance = a.at(1).get<int>();
    v.generationCenterChunkX       = a.at(2).get<int>();
    v.generationCenterChunkZ       = a.at(3).get<int>();
    v.generationMaxChunkRadius     = a.at(4).get<int>();
    v.generationRequestRateLimit   = a.at(5).get<int>();
    v.enableRealTimeUpdates        = a.at(6).get<bool>();
    v.realTimeUpdateDistanceRadius = a.at(7).get<int>();
    v.synchronizeOnLoad            = a.at(8).get<bool>();
    v.maxSyncOnLoadRequestDistance = a.at(9).get<int>();
    v.syncOnLoadRateLimit          = a.at(10).get<int>();
    v.playerBandwidthLimit         = a.at(11).get<int>();
    return v;
}

} // namespace

TEST_CASE("golden net: messages and session constrain") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "net");
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx           c(gc, st);
        const std::string kind = gc.name.substr(0, gc.name.rfind('_'));
        if (kind == "codec_crash") {
            ++st.skipped; // id 11 not ported
            continue;
        }
        if (kind == "session_constrain") {
            const SessionConfigValues a = fromArray(gc.in.at("a")), b = fromArray(gc.in.at("b"));
            // "X constrained by Y" = constrain(client = Y, server = X): the server's own value wins where the
            // rule says "X's own value" (see README).
            const SessionConfigValues ab = constrain(b, a), ba = constrain(a, b);
            auto                      arr = [](const SessionConfigValues& v) {
                return std::array<std::int64_t, 12>{
                    static_cast<int>(v.generatorPlan),
                    v.maxGenerationRequestDistance,
                    v.generationCenterChunkX,
                    v.generationCenterChunkZ,
                    v.generationMaxChunkRadius,
                    v.generationRequestRateLimit,
                    v.enableRealTimeUpdates,
                    v.realTimeUpdateDistanceRadius,
                    v.synchronizeOnLoad,
                    v.maxSyncOnLoadRequestDistance,
                    v.syncOnLoadRateLimit,
                    v.playerBandwidthLimit
                };
            };
            auto cmp = [&](const char* key, const SessionConfigValues& v) {
                const auto  got = arr(v);
                const auto& e   = gc.out.at(key);
                ++st.checked;
                INFO("case ", gc.name, " key ", key);
                REQUIRE(e.size() == 12);
                for (std::size_t i = 0; i < 12; ++i) {
                    INFO("field ", i);
                    const std::int64_t ev = e[i].is_boolean() ? (e[i].get<bool>() ? 1 : 0) : e[i].get<std::int64_t>();
                    CHECK_EQ(got[i], ev);
                }
            };
            cmp("aConstrainedByB", ab);
            cmp("bConstrainedByA", ba);
            std::vector<std::byte> enc;
            ByteWriter             w(enc);
            ab.encode(w);
            c.eq("aConstrainedByB_encoded", toHexStr(enc));
            continue;
        }

        // message case
        const std::vector<std::byte> javaFrame = fromHex(gc.out.at("frame").get<std::string>());
        const std::vector<std::byte> javaBody  = fromHex(gc.out.at("body").get<std::string>());
        REQUIRE(javaFrame.size() >= 4);
        c.eq("id", static_cast<int>((std::to_integer<int>(javaFrame[2]) << 8) | std::to_integer<int>(javaFrame[3])));
        ++st.checked; // frame = i16 16 + u16 id + body (Java side of the schema)
        CHECK_EQ(toHexStr(std::vector<std::byte>(javaFrame.begin() + 4, javaFrame.end())), toHexStr(javaBody));

        std::vector<std::byte> ours = javaFrame;
        ours[0]                     = static_cast<std::byte>(kProtocolVersion >> 8);
        ours[1]                     = static_cast<std::byte>(kProtocolVersion & 0xFF);
        auto r                      = decodeFrame(ours);
        INFO("case ", gc.name);
        c.eq("roundTrip", r.has_value()); // Java: decode(body) consumed everything and re-encoded identically
        if (!r.has_value()) {
            // The roundTrip CHECK above already failed; body/frame cannot be compared without a decoded message.
            ++st.skipped;
            continue;
        }
        const std::vector<std::byte> re = encodeFrame(*r.value());
        c.eq("id", static_cast<int>(r.value()->id()));
        ++st.checked;
        CHECK_EQ(toHexStr(std::vector<std::byte>(re.begin() + 4, re.end())), toHexStr(javaBody));
        ++st.checked;
        CHECK_EQ(toHexStr(re), toHexStr(ours));
    }
    gv::report("net", st);
}
