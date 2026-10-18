// Golden replays for DhLightingEngine (tests/golden/{light_chunk,light_ds}.gv.json).
//
// light_chunk: in.chunks[{dx,dz,chunk}] (absent neighbours omitted, list order as dumped) ->
//   lightChunk(center, nearby, maxSkyLight). Keys checked: skyCorrect, blockCorrect, colTable/colRuns (the centre's
//   sky*16+block array for every column, y = minY..maxY-1).
// light_ds: bakeDataSourceSkyLight(source, maxSkyLight); out.cols is compared for the listed columns, and every
//   unlisted column must stay as it was in the input.
// SKIPPED_KEYS: none. No `{"throws"}` values exist in these suites.
#include <doctest.h>

#include "dh/build/LightingEngine.h"
#include "golden_check.h"
#include "golden_fixtures.h"

using namespace dh;
using namespace dh::test;
using gf::json;

TEST_CASE("golden light_chunk: DhLightingEngine.lightChunk") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "light_chunk");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);

        const int cx = gc.in.at("centerChunkX").get<int>(), cz = gc.in.at("centerChunkZ").get<int>();
        std::vector<std::unique_ptr<TestChunk>> owned;
        std::vector<IChunk*>                    nearby;
        IChunk*                                 center = nullptr;
        for (const json& e : gc.in.at("chunks")) {
            owned.push_back(gf::fromGoldenChunk(e.at("chunk"), *reg));
            nearby.push_back(owned.back().get());
            if (e.at("dx").get<int>() == 0 && e.at("dz").get<int>() == 0) center = owned.back().get();
        }
        REQUIRE(center != nullptr);
        REQUIRE_EQ(center->chunkX(), cx);
        REQUIRE_EQ(center->chunkZ(), cz);

        LightingOptions opts;
        opts.maxSkyLight = gc.in.at("maxSkyLight").get<int>();
        lightChunk(*center, nearby, opts);

        c.eq("skyCorrect", center->isDhSkyLightingCorrect());
        c.eq("blockCorrect", center->isDhBlockLightingCorrect());
        ++st.checked; // colTable + colRuns
        CHECK_EQ(gf::diffLight(*center, gc.out), std::string());
    }
    gv::report("light_chk", st);
}

TEST_CASE("golden light_ds: DhLightingEngine.bakeDataSourceSkyLight") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "light_ds");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);
        auto src = gf::fromGoldenSource(gc.in.at("source"), *reg);

        std::vector<std::vector<std::uint64_t>> expected(src->columns.begin(), src->columns.end());
        for (const json& col : gc.out.at("cols")) expected[col.at(0).get<std::size_t>()] = gf::longs(col.at(1));

        bakeDataSourceSkyLight(*src, gc.in.at("maxSkyLight").get<int>());

        ++st.checked; // out.cols
        std::string diff;
        for (std::size_t i = 0; i < expected.size() && diff.empty(); ++i)
            if (src->columns[i] != expected[i])
                diff = "column " + std::to_string(i) + " C++ " + gf::colStr(src->columns[i]) + " vs Java "
                     + gf::colStr(expected[i]);
        CHECK_EQ(diff, std::string());
    }
    gv::report("light_ds", st);
}
