// Golden vectors for RenderDataPointReducingList via ColumnRenderView::changeVerticalSizeFrom
// (tests/golden/reduce.gv.json). Every out key (t1,t2,t3,t4,t6,t8,t16) is checked; SKIPPED_KEYS: none.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "dh/render/ColumnRenderSource.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

TEST_CASE("golden reduce: ReducingList / changeVerticalSizeFrom") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "reduce");
    gv::Stats            st;
    static constexpr int kTargets[] = {1, 2, 3, 4, 6, 8, 16};

    for (const GoldenCase& gc : s.cases) {
        ++st.cases;
        const std::vector<std::uint64_t> column = hexU64Array(gc.in.at("column"));
        const int                        n      = static_cast<int>(column.size());
        for (int t : kTargets) {
            const std::string          key = "t" + std::to_string(t);
            std::vector<std::uint64_t> src = column;
            std::vector<std::uint64_t> dst(static_cast<std::size_t>(t), 0xDEADBEEFDEADBEEFull);
            std::vector<std::uint64_t> voidS{0}, voidD{0};
            ColumnRenderView           sv, dv;
            REQUIRE(sv.populate(src, voidS, n, 0, 0, n));
            REQUIRE(dv.populate(dst, voidD, t, 0, 0, t));
            const bool ok = dv.changeVerticalSizeFrom(sv);
            INFO("case ", gc.name, " key ", key);
            CHECK(ok);
            const std::vector<std::uint64_t> expect = hexU64Array(gc.out.at(key));
            REQUIRE(expect.size() == static_cast<std::size_t>(t));
            ++st.checked;
            CHECK(dst == expect);
        }
    }
    gv::report("reduce", st);
}
