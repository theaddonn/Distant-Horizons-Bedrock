// Golden replays for the full-data merge chain (tests/golden/{merge_same,merge_up,downsample,cull}.gv.json).
//
// merge_same / merge_up / downsample: out = {dataChanged, target}. target is compared completely: pos,
//   isEmpty, mapping (serial pairs in id order, after removeUnusedIdsAndRemap), every column's datapoints,
//   genStep and compMode (defGen/defComp expanded). No `out` key is skipped.
// cull: out = {changed}; every column NOT listed must be unchanged, listed ones must equal the dumped list.
// SKIPPED_KEYS: none. No suite contains {"throws":...} values (a nullopt from updateFromDataSource is a failure).
#include <doctest.h>

#include "dh/fulldata/Merge.h"
#include "dh/fulldata/OcclusionCuller.h"
#include "golden_check.h"
#include "golden_fixtures.h"

using namespace dh;
using namespace dh::test;
using gf::json;

namespace {

void replayUpdate(const char* suiteName) {
    auto suiteOpt = loadGolden(suiteName);
    if (!suiteOpt) {
        MESSAGE("golden suite not found; skipping ", suiteName);
        return;
    }
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : suiteOpt->cases) {
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);

        UpdateOptions opts;
        opts.worldCompression = gc.in.at("worldCompression").get<int>() == 1 ? WorldCompressionMode::VisuallyEqual
                                                                             : WorldCompressionMode::MergeSameBlocks;
        auto target           = gf::fromGoldenSource(gc.in.at("target"), *reg);

        const json& inputs  = gc.in.at("inputs");
        const json& changed = gc.out.at("dataChanged");
        REQUIRE_EQ(changed.size(), inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto in = gf::fromGoldenSource(inputs[i], *reg);
            auto r  = updateFromDataSource(*target, *in, opts);
            ++st.checked;
            INFO("dataChanged[", i, "]");
            REQUIRE(r.has_value());
            CHECK_EQ(*r, changed[i].get<bool>());
        }

        const json& t = gc.out.at("target");
        c.eqSub("target", "pos", target->pos);
        c.eqSub("target", "isEmpty", target->isEmpty);
        ++st.checked;
        CHECK_EQ(gf::diffPalette(target->mapping, t.at("palette")), std::string());
        ++st.checked;
        CHECK_EQ(gf::diffColumns(*target, t), std::string());
    }
    gv::report(suiteName, st);
}

} // namespace

TEST_CASE("golden merge_same: updateFromDataSource at equal detail") { replayUpdate("merge_same"); }
TEST_CASE("golden merge_up: updateFromDataSource from four children") { replayUpdate("merge_up"); }
TEST_CASE("golden downsample: updateFromDataSource from the parent") { replayUpdate("downsample"); }

TEST_CASE("golden cull: cullHiddenDatapointsInColumn") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "cull");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);
        auto src = gf::fromGoldenSource(gc.in.at("source"), *reg);

        // expected = input with the `changed` columns replaced
        std::vector<std::vector<std::uint64_t>> expected(src->columns.begin(), src->columns.end());
        for (const json& ch : gc.out.at("changed")) expected[ch.at(0).get<std::size_t>()] = gf::longs(ch.at(1));

        for (int x = 0; x < FullDataSource::WIDTH; ++x)
            for (int z = 0; z < FullDataSource::WIDTH; ++z)
                if (src->column(x, z).size() > 1) cullHiddenDatapointsInColumn(*src, x, z);

        ++st.checked; // out.changed
        std::string diff;
        for (std::size_t i = 0; i < expected.size() && diff.empty(); ++i)
            if (src->columns[i] != expected[i])
                diff = "column " + std::to_string(i) + " C++ " + gf::colStr(src->columns[i]) + " vs Java "
                     + gf::colStr(expected[i]);
        CHECK_EQ(diff, std::string());
    }
    gv::report("cull", st);
}
