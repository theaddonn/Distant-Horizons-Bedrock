// Golden replay of ColumnRenderBufferBuilder.makeLodRenderData (tests/golden/mesh.gv.json).
// Every out key is checked: opaqueQuads, transparentQuads, opaque[] and transparent[] (buffer bytes as hex,
// buffer count included). SKIPPED_KEYS: none.
// Reproduced on purpose: a coarser neighbour is not wrapped (acts as an empty view); dirtColor and shades
// come from the case input.
#include <doctest.h>

#include <string>

#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/pos/SectionPos.h"
#include "golden_check.h"
#include "golden_render_fixtures.h"

using namespace dh;
using namespace dh::test;

namespace {

std::string toHexStr(const std::vector<std::byte>& b) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    s.reserve(b.size() * 2);
    for (std::byte x : b) {
        s.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        s.push_back(d[std::to_integer<unsigned>(x) & 15]);
    }
    return s;
}

// The dump carries no `pos` (README says it does): only the detail level matters to the mesher. The centre is a
// detail-6 section; a non-null neighbour with sameDetail=false is the one-level-coarser section, else same detail.
std::uint64_t neighbourPos(std::size_t i, bool sameDetail) {
    static const int dx[4] = {0, 0, -1, 1}, dz[4] = {-1, 1, 0, 0};
    return sec::encode(sameDetail ? 6 : 7, 3 + dx[i], -2 + dz[i]);
}

} // namespace

TEST_CASE("golden mesh: makeLodRenderData") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "mesh");
    gr::RenderRegistry* reg = gr::RenderRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx     c(gc, st);
        const auto& in = gc.in;
        INFO("case ", gc.name);
        const auto& jo = in.at("options");

        MeshOptions o;
        o.quad.transparency = jo.at("transparency").get<int>() == 0 ? Transparency::Disabled : Transparency::Complete;
        o.quad.grassSide    = static_cast<GrassSideRendering>(jo.at("grassSideRendering").get<int>());
        o.quad.debugRenderMode           = false;
        o.quad.earthCurveEnabled         = false;
        o.quad.showOverlappingQuadErrors = false;
        o.quad.dirtColorArgb             = static_cast<std::uint32_t>(in.at("dirtColor").get<std::int64_t>());
        const auto& jsh                  = in.at("shades");
        const char* faceNames[6]         = {"DOWN", "UP", "NORTH", "SOUTH", "WEST", "EAST"};
        for (int f = 0; f < 6; ++f) o.quad.shadeByFace[static_cast<std::size_t>(f)] = jsh.at(faceNames[f]).get<float>();
        o.enableCaveCulling    = jo.at("enableCaveCulling").get<bool>();
        o.caveCullingHeight    = jo.at("caveCullingHeight").get<int>();
        o.brightnessMultiplier = jo.at("brightnessMultiplier").get<float>();
        o.saturationMultiplier = jo.at("saturationMultiplier").get<float>();
        o.debugRendering       = static_cast<DebugRendering>(jo.at("debugRendering").get<int>());

        gr::DumperLevel level(*reg, in.at("level").at("minHeight").get<int>(), 320, false, false);
        level.shades = o.quad.shadeByFace;

        const ColumnRenderSource        centre = gr::renderSourceFromJson(in.at("center"), sec::encode(6, 3, -2));
        std::vector<ColumnRenderSource> neigh;
        neigh.reserve(4);
        AdjacentSources  adj;
        std::vector<int> present;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& jn = in.at("neighbours")[i];
            if (jn.is_null()) continue;
            neigh.push_back(gr::renderSourceFromJson(jn, neighbourPos(i, in.at("sameDetail")[i].get<bool>())));
            present.push_back(static_cast<int>(i));
        }
        for (std::size_t k = 0; k < present.size(); ++k) adj.bySide[static_cast<std::size_t>(present[k])] = &neigh[k];
        for (std::size_t i = 0; i < 4; ++i) adj.sameDetail[i] = in.at("sameDetail")[i].get<bool>();

        LodQuadBuilder b(o.quad);
        ++st.checked;
        CHECK(makeLodRenderData(b, centre, level, adj, o));
        c.eq("opaqueQuads", b.getCurrentOpaqueQuadsCount());
        c.eq("transparentQuads", b.getCurrentTransparentQuadsCount());

        const MeshBuffers m  = b.build();
        const auto&       eo = gc.out.at("opaque");
        const auto&       et = gc.out.at("transparent");
        ++st.checked;
        CHECK_EQ(m.opaque.size(), eo.size());
        ++st.checked;
        CHECK_EQ(m.transparent.size(), et.size());
        for (std::size_t i = 0; i < eo.size() && i < m.opaque.size(); ++i) {
            ++st.checked;
            INFO("key opaque[", i, "]");
            CHECK_EQ(toHexStr(m.opaque[i]), eo[i].get<std::string>());
        }
        for (std::size_t i = 0; i < et.size() && i < m.transparent.size(); ++i) {
            ++st.checked;
            INFO("key transparent[", i, "]");
            CHECK_EQ(toHexStr(m.transparent[i]), et[i].get<std::string>());
        }
    }
    gv::report("mesh", st);
}
