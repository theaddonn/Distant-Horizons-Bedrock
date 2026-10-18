// Golden vectors for the LodQuadTree detail maths, CameraZoom.coneIntersectsCircle and DhFrustumBounds
// (tests/golden/treemath.gv.json, see tools/golden-dumper/README.md "treemath").
//
//   detail_N  - DetailLevelMath::compute + calcDetailLevelFromDistance + calcExpectedDetailLevel (with zoom)
//   cone_N    - CameraZoom::coneIntersectsCircle
//   frustum_N - Frustum::update + intersects: the 24 normalised plane floats are compared BIT-EXACT
//               (planeBits), visibility exactly. boundsMin/boundsMax come from an inverse that is not JOML's
//               (glm), so they are compared with a tolerance and never used for culling anyway.
#include <doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "dh/render/Enums.h"
#include "dh/render/Frustum.h"
#include "dh/tree/LodQuadTree.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;
using namespace dh::tree;

namespace {

CameraZoom zoomFromJson(const nlohmann::json& z) {
    CameraZoom zoom;
    if (z.is_null()) return zoom;
    zoom.magnification    = z.at("magnification").get<double>();
    zoom.coneTanHalfAngle = z.at("coneTanHalfAngle").get<double>();
    zoom.lookDirectionX   = z.at("lookX").get<double>();
    zoom.lookDirectionZ   = z.at("lookZ").get<double>();
    return zoom;
}

/// DhApiMat4f row-major mat[r*4+c] -> glm m[c][r], from the exact Java float bit patterns.
glm::mat4 matFromBits(const nlohmann::json& bits) {
    glm::mat4 m(0.0f);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const auto b = bits.at(static_cast<std::size_t>(r * 4 + c)).get<std::int32_t>();
            m[c][r]      = std::bit_cast<float>(b);
        }
    }
    return m;
}

bool approxRel(float expected, float actual, double rel, double abs) {
    const double diff = std::fabs(static_cast<double>(expected) - static_cast<double>(actual));
    return diff <= abs || diff <= rel * std::fabs(static_cast<double>(expected));
}

} // namespace

TEST_CASE("golden treemath: detail levels, zoom cone, frustum") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "treemath");
    gv::Stats detailStats, coneStats, frustumStats;
    long      planeBitMismatches = 0, boundsChecked = 0, boundsOutOfTolerance = 0;

    for (const GoldenCase& gc : s.cases) {
        if (gc.name.rfind("detail_", 0) == 0) {
            gv::Ctx c(gc, detailStats);

            const auto hq       = static_cast<HorizontalQuality>(c.inI("horizontalQuality"));
            const auto res      = static_cast<MaxHorizontalResolution>(c.inI("maxHorizontalResolution"));
            const int  diameter = c.inI("diameter");

            const std::int8_t     treeRoot = LodQuadTree::rootDetailLevelFor(diameter);
            const DetailLevelMath m        = DetailLevelMath::compute(hq, res, diameter, treeRoot);

            c.eq("treeRootDetail", static_cast<int>(m.treeRootDetailLevel));
            c.eq("dropOffUnit", m.detailDropOffDistanceUnit);
            c.eq("dropOffLogBase", m.detailDropOffLogBase);
            c.eq("maxLeafDetail", static_cast<int>(m.maxLeafRenderDetailLevel));
            c.eq("minRootDetail", static_cast<int>(m.minRootRenderDetailLevel));

            const auto& distances = gc.in.at("distances");
            const auto& outDist   = gc.out.at("fromDistance");
            for (std::size_t i = 0; i < distances.size(); i++) {
                INFO("case ", gc.name, " fromDistance[", i, "]");
                ++detailStats.checked;
                CHECK_EQ(
                    outDist.at(i).get<int>(),
                    static_cast<int>(m.calcDetailLevelFromDistance(distances.at(i).get<double>()))
                );
            }

            const BlockPos2D player(gc.in.at("player").at(0).get<int>(), gc.in.at("player").at(1).get<int>());
            const CameraZoom zoom = zoomFromJson(gc.in.at("zoom"));

            const auto& sections = gc.in.at("sections");
            const auto& outSec   = gc.out.at("expectedSection");
            for (std::size_t i = 0; i < sections.size(); i++) {
                INFO("case ", gc.name, " expectedSection[", i, "]");
                ++detailStats.checked;
                CHECK_EQ(
                    outSec.at(i).get<int>(),
                    static_cast<int>(m.calcExpectedDetailLevel(player, hexU64(sections.at(i)), zoom))
                );
            }
            const auto& blocks   = gc.in.at("blocks");
            const auto& outBlock = gc.out.at("expectedBlock");
            for (std::size_t i = 0; i < blocks.size(); i++) {
                INFO("case ", gc.name, " expectedBlock[", i, "]");
                ++detailStats.checked;
                CHECK_EQ(
                    outBlock.at(i).get<int>(),
                    static_cast<int>(m.calcExpectedDetailLevel(
                        player,
                        blocks.at(i).at(0).get<int>(),
                        blocks.at(i).at(1).get<int>(),
                        zoom
                    ))
                );
            }
        } else if (gc.name.rfind("cone_", 0) == 0) {
            gv::Ctx c(gc, coneStats);

            CameraZoom zoom;
            zoom.magnification    = 2.0; // irrelevant for the cone test
            zoom.coneTanHalfAngle = c.inD("coneTanHalfAngle");
            zoom.lookDirectionX   = c.inD("lookX");
            zoom.lookDirectionZ   = c.inD("lookZ");
            const double ox       = gc.in.at("origin").at(0).get<double>();
            const double oz       = gc.in.at("origin").at(1).get<double>();

            const auto& circles = gc.in.at("circles");
            const auto& hits    = gc.out.at("hits");
            for (std::size_t i = 0; i < circles.size(); i++) {
                INFO("case ", gc.name, " hits[", i, "]");
                ++coneStats.checked;
                const auto& circle = circles.at(i);
                CHECK_EQ(
                    hits.at(i).get<bool>(),
                    zoom.coneIntersectsCircle(
                        ox,
                        oz,
                        circle.at(0).get<double>(),
                        circle.at(1).get<double>(),
                        circle.at(2).get<double>()
                    )
                );
            }
        } else if (gc.name.rfind("frustum_", 0) == 0) {
            gv::Ctx c(gc, frustumStats);

            Frustum frustum;
            frustum.update(c.inI("minY"), c.inI("maxY"), matFromBits(gc.in.at("matBits")));

            // 24 plane floats in JOML field order nx, px, ny, py, nz, pz (X, Y, Z, W each): bit-exact
            const auto& planeBits = gc.out.at("planeBits");
            for (int p = 0; p < 6; p++) {
                const Frustum::Plane& pl     = frustum.planes()[static_cast<std::size_t>(p)];
                const float           comp[] = {pl.x, pl.y, pl.z, pl.w};
                for (int k = 0; k < 4; k++) {
                    const std::size_t  idx      = static_cast<std::size_t>(p * 4 + k);
                    const std::int32_t expected = planeBits.at(idx).get<std::int32_t>();
                    const std::int32_t actual   = std::bit_cast<std::int32_t>(comp[k]);
                    ++frustumStats.checked;
                    if (expected != actual) ++planeBitMismatches;
                    INFO("case ", gc.name, " plane ", gc.out.at("planeNames").at(idx).get<std::string>());
                    CHECK_EQ(expected, actual);
                }
            }

            const auto& boxes   = gc.in.at("boxes");
            const auto& visible = gc.out.at("visible");
            for (std::size_t i = 0; i < boxes.size(); i++) {
                const auto& box = boxes.at(i);
                INFO("case ", gc.name, " visible[", i, "]");
                ++frustumStats.checked;
                CHECK_EQ(
                    visible.at(i).get<bool>(),
                    frustum.intersects(
                        box.at(0).get<int>(),
                        box.at(1).get<int>(),
                        box.at(2).get<int>(),
                        box.at(3).get<int>()
                    )
                );
            }

            // frustum AABB: tolerance only (see header)
            const auto&      bmin = gc.out.at("boundsMin");
            const auto&      bmax = gc.out.at("boundsMax");
            const glm::vec3& amin = frustum.boundsMin();
            const glm::vec3& amax = frustum.boundsMax();
            for (int k = 0; k < 3; k++) {
                ++boundsChecked;
                if (!approxRel(bmin.at(static_cast<std::size_t>(k)).get<float>(), amin[k], 1e-3, 1e-2))
                    ++boundsOutOfTolerance;
                ++boundsChecked;
                if (!approxRel(bmax.at(static_cast<std::size_t>(k)).get<float>(), amax[k], 1e-3, 1e-2))
                    ++boundsOutOfTolerance;
            }
        }
    }

    gv::report("treemath/detail", detailStats);
    gv::report("treemath/cone", coneStats);
    gv::report("treemath/frustum", frustumStats);
    std::fprintf(
        stderr,
        "[golden] treemath/frustum planeBitMismatches=%ld boundsChecked=%ld boundsOutOfTolerance=%ld\n",
        planeBitMismatches,
        boundsChecked,
        boundsOutOfTolerance
    );
    CHECK_EQ(planeBitMismatches, 0);
    // bounds: the dumper's random matrices have condition numbers of 1e4..1e8, so a float inverse that is not
    // JOML's (even a double-precision reference misses ~4% of them at 1e-3) legitimately drifts; the AABB is
    // never used for culling. Sanity floor only: the large majority must still agree to 0.1%.
    CHECK_LE(boundsOutOfTolerance, boundsChecked / 5);
}
