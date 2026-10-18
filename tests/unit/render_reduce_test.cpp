// Tests for WP-E1: ReducingList (Java RenderDataPointReducingList) and the shrinking path of
// ColumnRenderView::changeVerticalSizeFrom.
// The Java repo has no unit tests for either (grep of core/src/test/java/tests for
// ReducingList / changeVerticalSize finds none), so the cases below are hand-built from the
// Java source, a structural-invariant fuzz, and the golden suite tests/golden/reduce.gv.json.
// The hand-built expectations were cross-checked by running the original Java.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "dh/render/ColumnRenderSource.h"
#include "dh/render/ReducingList.h"
#include "dh/render/RenderDataPoint.h"
#include "golden_loader.h"

using dh::BlockMaterial;
using dh::ColumnRenderView;
using dh::ReducingList;
namespace rdp = dh::rdp;

namespace {

using Col = std::vector<std::uint64_t>;

constexpr std::uint32_t RED   = 0xFFAA2222u;
constexpr std::uint32_t BLUE  = 0xFF2222AAu;
constexpr std::uint32_t GREEN = 0xFF22AA22u;
constexpr std::uint32_t GLASS = 0x8033CCCCu; // alpha nibble 8
constexpr std::uint32_t FOG   = 0x40CCCCCCu; // alpha nibble 4

std::uint64_t
pt(int maxY, int minY, std::uint32_t argb, int sky = 15, int blk = 0, BlockMaterial m = BlockMaterial::Unknown) {
    return rdp::create(maxY, minY, argb, sky, blk, m);
}

/// Runs ColumnRenderView::changeVerticalSizeFrom on one column (source has col.size() slices,
/// destination `target` slices). nullopt = the call returned false (Java would throw).
std::optional<Col> shrink(const Col& col, int target) {
    Col src = col;
    Col dst(static_cast<std::size_t>(target), 0xDEADBEEFDEADBEEFULL); // poison: every slot must be written
    Col voidS{0x1234};
    Col voidD{0};

    ColumnRenderView sv, dv;
    const int        n = static_cast<int>(col.size());
    REQUIRE(sv.populate(src, voidS, n, 0, 0, n));
    REQUIRE(dv.populate(dst, voidD, target, 0, 0, target));
    if (!dv.changeVerticalSizeFrom(sv)) {
        return std::nullopt;
    }
    CHECK(voidD[0] == 0x1234); // void data is copied over
    return dst;
}

/// The visible (non-void) prefix of a reduced column.
Col visible(const Col& c) {
    Col out;
    for (const std::uint64_t d : c) {
        if (d != rdp::EMPTY_DATA) {
            out.push_back(d);
        }
    }
    return out;
}

} // namespace

TEST_CASE("reduce: smallest two adjacent same-alpha segments merge, the upper one wins") {
    // red(4..5) and blue(3..4) are both opaque, green(0..3) is bigger: blue is swallowed by red.
    const Col  in  = {pt(5, 4, RED), pt(4, 3, BLUE), pt(3, 0, GREEN)};
    const auto out = shrink(in, 2);
    REQUIRE(out.has_value());
    CHECK(*out == Col{pt(5, 3, RED), pt(3, 0, GREEN)});
}

TEST_CASE("reduce: exactly-target-size column is untouched") {
    // 3 visible points in a 5 slot column, target 3: nothing is merged, tail is void.
    const Col  in  = {pt(30, 20, RED), pt(20, 10, BLUE), pt(10, 0, GREEN), 0, 0};
    const auto out = shrink(in, 3);
    // 5 -> 3 slots goes through the list
    REQUIRE(out.has_value());
    CHECK(*out == Col{in[0], in[1], in[2]});

    // same with air gaps between the points: gaps do not count towards the size
    const Col  gaps = {pt(40, 30, RED), pt(20, 10, BLUE), pt(6, 0, GREEN), 0, 0};
    const auto out2 = shrink(gaps, 3);
    REQUIRE(out2.has_value());
    CHECK(*out2 == Col{gaps[0], gaps[1], gaps[2]});
}

TEST_CASE("reduce: equal or growing slot counts just copy") {
    const Col in = {pt(30, 20, RED), pt(20, 10, BLUE)};
    CHECK(*shrink(in, 2) == in);
    CHECK(*shrink(in, 4) == Col{in[0], in[1], 0, 0});
}

TEST_CASE("reduce: air gaps are kept as gaps, not stretched over") {
    const Col        in = {pt(40, 30, RED), pt(10, 0, BLUE), 0};
    ColumnRenderView v;
    Col              c = in, vd{0};
    REQUIRE(v.populate(c, vd, 3, 0, 0, 3));
    ReducingList list;
    REQUIRE(list.populate(v));
    CHECK(list.size() == 2);        // visible
    CHECK(list.sizeWithAir() == 3); // + one air node
    CHECK(list.checkLinks());
    list.reduce(2);
    CHECK(list.size() == 2);
    Col              o(3, 99);
    ColumnRenderView ov;
    REQUIRE(ov.populate(o, vd, 3, 0, 0, 3));
    list.copyTo(ov);
    CHECK(o == Col{in[0], in[1], 0}); // top-down, original gap intact
}

TEST_CASE("reduce: reduceToOne keeps the top point's properties and the lowest min Y") {
    const Col  in  = {pt(50, 40, RED, 3, 4, BlockMaterial::Water), pt(30, 20, BLUE), pt(10, 0, GREEN)};
    const auto out = shrink(in, 1);
    REQUIRE(out.has_value());
    CHECK(*out == Col{pt(50, 0, RED, 3, 4, BlockMaterial::Water)});

    // invisible and void points are ignored
    const Col in2 = {pt(60, 55, 0x00FFFFFFu), pt(50, 40, BLUE), pt(30, 20, GLASS), 0};
    CHECK(*shrink(in2, 1) == Col{pt(50, 20, BLUE)});

    // nothing visible -> void
    const Col in3 = {pt(60, 55, 0x00FFFFFFu), 0, 0};
    CHECK(*shrink(in3, 1) == Col{0});

    // Java quirk (else-if): a new highest is never also tested as the lowest.
    // 1st point (20..30) sets both; 2nd (40..50) becomes highest; 3rd (0..10) becomes lowest.
    const Col        q  = {pt(30, 20, RED), pt(50, 40, BLUE), pt(10, 0, GREEN)};
    Col              qc = q, qv{0};
    ColumnRenderView qview;
    REQUIRE(qview.populate(qc, qv, 3, 0, 0, 3));
    CHECK(ReducingList::reduceToOne(qview) == pt(50, 0, BLUE));
}

TEST_CASE("reduce: opaque beats nothing, transparent sandwiched between equal alpha is erased") {
    // stone(30..40) / glass(20..30) / stone2(10..20), target 2: the glass is erased and the stones fuse.
    const Col  in  = {pt(40, 30, RED), pt(30, 20, GLASS), pt(20, 10, GREEN), 0};
    const auto out = shrink(in, 2);
    // 4 -> 2 slots: only one visible segment survives; it keeps the TOP stone's colour
    REQUIRE(out.has_value());
    CHECK(*out == Col{pt(40, 10, RED), 0});
}

TEST_CASE("reduce: forced merge of the bottom into the next visible segment") {
    // no equal-alpha neighbours anywhere: step 3 removes the lowest segment, the one above extends down.
    const Col  in  = {pt(30, 20, GLASS), pt(20, 10, RED), pt(10, 0, FOG)};
    const auto out = shrink(in, 2);
    REQUIRE(out.has_value());
    CHECK(*out == Col{pt(30, 20, GLASS), pt(20, 0, RED)});
}

TEST_CASE("reduce: overlapping visible points are rejected (Java IllegalArgumentException)") {
    const Col in = {pt(30, 20, RED), pt(25, 10, BLUE), pt(5, 0, GREEN)};
    CHECK_FALSE(shrink(in, 2).has_value());
}

TEST_CASE("reduce: empty and all-invisible columns produce a void column") {
    CHECK(*shrink(Col{0, 0, 0, 0}, 2) == Col{0, 0});
    CHECK(*shrink(Col{pt(9, 3, 0x00112233u), 0, 0}, 2) == Col{0, 0});
}

namespace {

struct Gen {
    std::mt19937_64 rng;
    int uni(int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<std::uint64_t>(hi - lo + 1)); }

    /// top-down column of n slots: random heights/gaps/colours/lights/materials, some invisible/void slots.
    Col column(int n) {
        static const std::uint32_t colors[] = {0xFF112233u, 0xFF445566u, 0xFF778899u, 0xFFAABBCCu};
        static const int           alphas[] = {255, 255, 255, 128, 64, 0};
        Col                        bottomUp;
        int                        y    = uni(0, 3);
        const int                  mode = uni(0, 3);
        for (int i = 0; i < n; ++i) {
            const int           h = mode == 0 ? uni(1, 2) : mode == 1 ? uni(1, 6) : mode == 2 ? uni(1, 40) : uni(1, 3);
            const int           gap = uni(0, 2) == 0 ? uni(0, mode == 2 ? 30 : 3) : 0;
            const int           mn  = y + gap;
            const int           mx  = std::min(mn + h, 4000);
            const std::uint32_t c =
                (colors[uni(0, mode == 3 ? 1 : 3)] & 0xFFFFFFu) | (static_cast<std::uint32_t>(alphas[uni(0, 5)]) << 24);
            bottomUp.push_back(
                rdp::create(mx, std::min(mn, mx - 1), c, uni(0, 15), uni(0, 15), static_cast<BlockMaterial>(uni(0, 15)))
            );
            y = mx;
        }
        std::reverse(bottomUp.begin(), bottomUp.end());
        return bottomUp;
    }
};

} // namespace

TEST_CASE("reduce: fuzz structural invariants") {
    Gen          g{std::mt19937_64(0xD157A17ULL)};
    const int    targets[] = {1, 2, 3, 4, 6, 8, 16};
    ReducingList list;
    for (int iter = 0; iter < 4000; ++iter) {
        const int n      = g.uni(1, 64);
        const int target = targets[g.uni(0, 6)];
        const Col in     = g.column(n);

        // span of the visible, non-degenerate input
        int lo = 1 << 20, hi = -1;
        for (const std::uint64_t d : in) {
            if (ReducingList::isDataVisible(d) && rdp::yMin(d) < rdp::yMax(d)) {
                lo = std::min(lo, rdp::yMin(d));
                hi = std::max(hi, rdp::yMax(d));
            }
        }

        // direct list use: also verify internal link consistency after reduce
        Col              src = in, vd{0};
        ColumnRenderView sv;
        REQUIRE(sv.populate(src, vd, n, 0, 0, n));
        REQUIRE(list.populate(sv));
        REQUIRE(list.checkLinks());
        list.reduce(target);
        REQUIRE(list.checkLinks());
        CHECK(list.size() <= std::max(target, 0));

        const auto out = shrink(in, target);
        REQUIRE(out.has_value());
        if (n <= target) {
            continue; // grow/equal path: plain copy, covered elsewhere
        }
        REQUIRE(static_cast<int>(out->size()) == target);
        const Col vis = visible(*out);
        CHECK(static_cast<int>(vis.size()) <= target);
        // the void slots are a tail
        bool seenVoid = false;
        for (const std::uint64_t d : *out) {
            if (d == rdp::EMPTY_DATA) {
                seenVoid = true;
            } else {
                CHECK_FALSE(seenVoid);
            }
        }
        if (hi < 0) {
            CHECK(vis.empty());
            continue;
        }
        CHECK_FALSE(vis.empty());
        int prevMin = 1 << 20;
        for (const std::uint64_t d : vis) {
            CHECK(ReducingList::isDataVisible(d));
            CHECK(rdp::yMin(d) < rdp::yMax(d));
            CHECK(rdp::yMin(d) >= lo);
            CHECK(rdp::yMax(d) <= hi);
            CHECK(rdp::yMax(d) <= prevMin); // top-down, no overlap
            prevMin = rdp::yMin(d);
        }
    }
}

TEST_CASE("reduce: golden vectors (reduce.gv.json)") {
    DH_GOLDEN_SUITE_OR_SKIP(suite, "reduce.pending");
    // Key mapping assumed (re-check against tools/golden-dumper/README.md when the suite lands):
    //   in.column  = hex-long array, the source column (top-down), n = its length
    //   in.target  = destination slot count
    //   out.column = hex-long array of `target` entries, or out.throws when Java rejected the input
    for (const auto& c : suite.cases) {
        INFO("case " << c.name);
        const Col  in     = dh::test::hexU64Array(c.in["column"]);
        const int  target = c.in["target"].get<int>();
        const auto got    = shrink(in, target);
        if (c.out.contains("throws")) {
            CHECK_FALSE(got.has_value());
            continue;
        }
        REQUIRE(got.has_value());
        CHECK(*got == dh::test::hexU64Array(c.out["column"]));
    }
}
