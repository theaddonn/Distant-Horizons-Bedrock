// Golden vectors for FullDataPointUtil (tests/golden/datapoint.gv.json).
//
// SKIPPED_KEYS (the C++ port has no validation, hence no rejecting variant):
//   encode            - only the cases where Java returns a value are compared; DataCorruptedException /
//                       AssertFailureException cases are skipped (and the encoded_* keys are absent there).
//   validateDatapoint - no C++ equivalent at all (every case skipped, value or throw).
#include <doctest.h>

#include "dh/fulldata/FullDataPoint.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

TEST_CASE("golden datapoint: FullDataPointUtil") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "datapoint");
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);

        // Java: encode(int id, int height, int relMinY, byte blockLight, byte skyLight)
        const auto bl = static_cast<std::uint8_t>(static_cast<std::int8_t>(c.inI("blockLight")));
        const auto sl = static_cast<std::uint8_t>(static_cast<std::int8_t>(c.inI("skyLight")));
        if (c.live("encode")) {
            c.eq("encode", fdp::encode(c.inI("id"), c.inI("height"), c.inI("minY"), bl, sl));
            const std::uint64_t enc = c.outHex("encode");
            c.eq("encoded_getId", fdp::id(enc));
            c.eq("encoded_getHeight", fdp::height(enc));
            c.eq("encoded_getBottomY", fdp::minY(enc));
            c.eq("encoded_getBlockLight", fdp::blockLight(enc));
            c.eq("encoded_getSkyLight", fdp::skyLight(enc));
        }

        const std::uint64_t raw = c.inHex("raw");
        c.eq("getId", fdp::id(raw));
        c.eq("getHeight", fdp::height(raw));
        c.eq("getBottomY", fdp::minY(raw));
        c.eq("getBlockLight", fdp::blockLight(raw));
        c.eq("getSkyLight", fdp::skyLight(raw));

        c.eq("setId", fdp::setId(raw, c.inI("newId")));
        c.eq("setHeight", fdp::setHeight(raw, c.inI("newHeight")));
        c.eq("setBottomY", fdp::setMinY(raw, c.inI("newMinY")));
        c.eq(
            "setBlockLight",
            fdp::setBlockLight(raw, static_cast<std::uint8_t>(static_cast<std::int8_t>(c.inI("newBlockLight"))))
        );
        c.eq("setSkyLight", fdp::setSkyLight(raw, c.inI("newSkyLight")));

        c.live("validateDatapoint"); // counted as skipped when Java returns/throws; no C++ validator
    }
    gv::report("datapoint", st);
}
