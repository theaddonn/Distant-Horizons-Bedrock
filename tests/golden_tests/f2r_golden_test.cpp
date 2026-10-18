// Golden replay of FullDataToRenderDataTransformer.transformFullDataToRenderSource (tests/golden/f2r.gv.json).
// Every out key is checked: maxVerticalSlices, yOffset, dataTable/dataRuns (all 4096 columns; the Java table
// trims trailing zero slots, so C++ columns are compared after trimming), voidTable/voidRuns (4096 void points).
// SKIPPED_KEYS: none.
// Dumper semantics reproduced here: the four CSVs go through the wrapper factory (unknown names -> air; ignore
// sets always contain air, replacement sets never do); level colours = the dumper's colour hash (see
// golden_render_fixtures.h).
#include <doctest.h>

#include "dh/render/FullToRender.h"
#include "golden_check.h"
#include "golden_fixtures.h"
#include "golden_render_fixtures.h"

using namespace dh;
using namespace dh::test;

namespace {

std::string colStr(const std::vector<std::uint64_t>& c) { return gf::colStr(c); }

std::vector<std::uint64_t> trimmed(std::span<const std::uint64_t> col) {
    std::size_t n = col.size();
    while (n > 0 && col[n - 1] == 0) --n;
    return {col.begin(), col.begin() + static_cast<std::ptrdiff_t>(n)};
}

} // namespace

TEST_CASE("golden f2r: transformFullDataToRenderSource") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "f2r.pending");
    gr::RenderRegistry* reg = gr::RenderRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx     c(gc, st);
        const auto& in = gc.in;
        INFO("case ", gc.name);

        const auto& jo = in.at("options");
        const auto& jl = in.at("level");

        BlockSet ignored     = resolveCsv(jo.at("ignoredRenderBlockCsv").get<std::string>(), *reg, true);
        BlockSet ignoredCave = resolveCsv(jo.at("ignoredRenderCaveBlockCsv").get<std::string>(), *reg, true);
        BlockSet surfaceRepl = resolveCsv(jo.at("waterSurfaceBlockReplacementCsv").get<std::string>(), *reg, false);
        BlockSet subsurfRepl = resolveCsv(jo.at("waterSubSurfaceBlockReplacementCsv").get<std::string>(), *reg, false);
        ignored.insert(reg->air());
        ignoredCave.insert(reg->air());

        TransformOptions o;
        o.verticalQuality = static_cast<VerticalQuality>(jo.at("verticalQuality").get<int>());
        o.blocksToAvoid   = jo.at("blocksToIgnore").get<int>() == 0 ? BlocksToAvoid::None : BlocksToAvoid::NonColliding;
        o.tintWithAvoidedBlocks  = jo.at("tintWithAvoidedBlocks").get<bool>();
        o.enableCaveCulling      = jo.at("enableCaveCulling").get<bool>();
        o.caveCullingHeight      = jo.at("caveCullingHeight").get<int>();
        o.ignoredBlocks          = &ignored;
        o.ignoredCaveBlocks      = &ignoredCave;
        o.waterSurfaceReplace    = &surfaceRepl;
        o.waterSubSurfaceReplace = &subsurfRepl;

        gr::DumperLevel level(
            *reg,
            jl.at("minHeight").get<int>(),
            jl.at("maxHeight").get<int>(),
            jl.at("hasCeiling").get<bool>(),
            jl.at("isEnd").get<bool>()
        );

        auto src = gf::fromGoldenSource(in.at("source"), *reg);
        if (in.at("source").value("isEmpty", false)) src->isEmpty = true;

        auto r = transformFullDataToRenderSource(*src, level, *reg, o);
        ++st.checked;
        REQUIRE(r.has_value());
        const ColumnRenderSource& rs = *r;

        c.eq("maxVerticalSlices", rs.maxVerticalSlices);
        c.eq("yOffset", rs.yOffset);

        // expected columns
        const int                               slices = c.gc().out.at("maxVerticalSlices").get<int>();
        std::vector<std::vector<std::uint64_t>> table;
        for (const auto& t : gc.out.at("dataTable")) table.push_back(hexU64Array(t));
        std::string diff;
        int         n = 0;
        for (const auto& run : gc.out.at("dataRuns")) {
            const auto& t = table.at(run.at(0).get<std::size_t>());
            for (int k = 0; k < run.at(1).get<int>() && diff.empty(); ++k, ++n) {
                const int x = n / 64, z = n % 64;
                if (rs.maxVerticalSlices != slices || rs.data.size() < static_cast<std::size_t>(n + 1) * slices) {
                    diff = "C++ render source has too few slots";
                    break;
                }
                const auto got = trimmed(rs.column(x, z));
                if (got != t)
                    diff = "column x=" + std::to_string(x) + ",z=" + std::to_string(z) + " C++ " + colStr(got)
                         + " vs Java " + colStr(t);
            }
            if (!diff.empty()) break;
        }
        if (diff.empty() && n != 4096) diff = "dataRuns cover " + std::to_string(n) + " columns";
        ++st.checked;
        CHECK_EQ(diff, std::string());

        std::vector<std::uint64_t> vt = hexU64Array(gc.out.at("voidTable"));
        std::string                vdiff;
        int                        m = 0;
        for (const auto& run : gc.out.at("voidRuns")) {
            for (int k = 0; k < run.at(1).get<int>() && vdiff.empty(); ++k, ++m) {
                const std::uint64_t e = vt.at(run.at(0).get<std::size_t>());
                const std::uint64_t g =
                    static_cast<std::size_t>(m) < rs.voidData.size() ? rs.voidData[static_cast<std::size_t>(m)] : 0;
                if (g != e)
                    vdiff = "void x=" + std::to_string(m / 64) + ",z=" + std::to_string(m % 64) + " C++ " + gf::hx(g)
                          + " vs Java " + gf::hx(e);
            }
            if (!vdiff.empty()) break;
        }
        if (vdiff.empty() && m != 4096) vdiff = "voidRuns cover " + std::to_string(m) + " columns";
        ++st.checked;
        CHECK_EQ(vdiff, std::string());
    }
    gv::report("f2r", st);
}
