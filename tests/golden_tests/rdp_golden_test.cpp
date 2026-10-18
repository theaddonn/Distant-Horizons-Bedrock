// Golden vectors for RenderDataPointUtil (tests/golden/rdp.gv.json).
//
// SKIPPED_KEYS (the C++ port does not validate, hence has no rejecting variant):
//   createDataPoint, createDataPoint_color - only the cases where Java returns a value are compared;
//                       AssertFailureException cases are skipped (the getter keys are absent there).
#include <doctest.h>

#include "dh/render/RenderDataPoint.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

namespace {

void checkGetters(gv::Ctx& c, std::uint64_t dp, std::uint64_t other, int newMinY) {
    c.eq("getYMax", rdp::yMax(dp));
    c.eq("getYMin", rdp::yMin(dp));
    c.eq("getAlpha", rdp::alpha(dp));
    c.eq("getRed", rdp::red(dp));
    c.eq("getGreen", rdp::green(dp));
    c.eq("getBlue", rdp::blue(dp));
    c.eq("getLightSky", rdp::skyLight(dp));
    c.eq("getLightBlock", rdp::blockLight(dp));
    c.eq("getBlockMaterialId", rdp::materialId(dp));
    c.eq("getColor", static_cast<std::int32_t>(rdp::argb(dp)));
    c.eq("doesDataPointExist", rdp::doesDataPointExist(dp));
    c.eq("hasZeroHeight", rdp::hasZeroHeight(dp));
    c.eq("isVoid_derived", rdp::doesDataPointExist(dp) && rdp::hasZeroHeight(dp));
    c.eq("setYMin", rdp::setYMin(dp, newMinY));
    c.eq("compareDatapointPriority", rdp::comparePriority(dp, other));
    c.eq("compareDatapointPriority_rev", rdp::comparePriority(other, dp));
}

} // namespace

TEST_CASE("golden rdp: RenderDataPointUtil") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "rdp");
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx           c(gc, st);
        const std::string kind = gc.name.substr(0, gc.name.find('_'));
        if (kind == "rdp") {
            const auto mat = static_cast<BlockMaterial>(c.inI("materialId"));
            if (c.live("createDataPoint")) {
                c.eq(
                    "createDataPoint",
                    rdp::create(
                        c.inI("alpha"),
                        c.inI("red"),
                        c.inI("green"),
                        c.inI("blue"),
                        c.inI("maxY"),
                        c.inI("minY"),
                        c.inI("lightSky"),
                        c.inI("lightBlock"),
                        mat
                    )
                );
            }
            if (c.live("createDataPoint_color")) {
                c.eq(
                    "createDataPoint_color",
                    rdp::create(
                        c.inI("maxY"),
                        c.inI("minY"),
                        static_cast<std::uint32_t>(c.inI("color")),
                        c.inI("lightSky"),
                        c.inI("lightBlock"),
                        mat
                    )
                );
            }
            // Getters work on Java's packed value so a create mismatch does not cascade.
            if (c.has("getYMax")) {
                const char* src = !c.throws("createDataPoint") ? "createDataPoint" : "createDataPoint_color";
                checkGetters(c, c.outHex(src), c.inHex("other"), c.inI("newMinY"));
            }
        } else if (kind == "raw") {
            checkGetters(c, c.inHex("raw"), c.inHex("other"), c.inI("newMinY"));
        } else {
            FAIL("unhandled case kind " << gc.name);
        }
    }
    gv::report("rdp", st);
}
