// Golden replay for tests/golden/propagate.gv.json (WP-D3: SourceProvider + SourceUpdater + UpdatePropagator).
//
// Per case: MemoryLodStore + golden registry + ManualClock + InlineExecutor, propagator threadCount 4 (maxTasks 40),
// target (0,0). Each phase writes its entries through SourceUpdater::update (an update listener records the saved
// positions), then runs tick({0,0}) until positionsToPropagateUp/Down are both empty.
//
// STRICT: in.maxTasks == maxTaskCount(); out.sections (set and order by detail/x/z, pos, three flags, data.pos,
//   data.isEmpty, data.palette, all 4096 columns via runs).
// updateOrder (what Java defines): Java groups children by parent in a HashMap<Long, HashSet<Long>> and iterates it
//   in hash order (FullDataUpdatePropagatorV2.java:182-197), C++ uses first appearance in the nearest-first list,
//   so the ORDER is never asserted. For a phase with <= maxTasks input entries (no cap ambiguity) the saved
//   positions are compared as a MULTISET (same positions, same multiplicities). For phases with more entries
//   (the *_k8_* cases) the repo query is `ORDER BY Distance ASC LIMIT ?` (FullDataSourceV2Repo.java:449-458), so which
//   of the equal-distance leaves make the first batch is SQLite's unspecified tie order; only the SET of saved
//   positions is asserted and multiplicity differences are logged via MESSAGE.
// passes: never asserted (it depends on the same batching); a difference is logged via MESSAGE. C++ counts tick()
//   calls until both queues are empty.
// worldCompression: the dump ran with Config worldCompression = VISUALLY_EQUAL (leaf fill comp = 1), so the provider
//   MUST be configured with WorldCompressionMode::VisuallyEqual; the C++ default (MergeSameBlocks) gives different
//   columns.
// Speed: the replay is dominated by codec + sky-light work under ASan/UBSan. Cases costing > ~1.3 s (all *_k8_*
//   plus a few others, see kSlow) run only with env DH_GOLDEN_SLOW set; the number skipped is printed.
// Null mapping: the C++ store keeps flags as bits (null == false, see ILodStore.h StoreFlag). The dumped out.sections
//   never contain a null flag (all are true/false), so the question is moot on the output side; null on the input
//   side (applyToChildren / regenerateLeaf) maps to std::nullopt ("don't touch"), as in Java.
// passes: Java counted loop iterations of runParentUpdates+runChildUpdates until both queues were empty; C++ counts
//   tick() calls that were executed (the check for empty happens after each tick, like Java's do-while).
// SKIPPED_KEYS: none (updateOrder order and passes are informational, see above).
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#include "dh/platform/IClock.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/storage/SourceProvider.h"
#include "dh/storage/UpdatePropagator.h"
#include "dh/thread/InlineExecutor.h"
#include "golden_check.h"
#include "golden_fixtures.h"

using namespace dh;
using namespace dh::test;
using gf::json;

namespace {

/// runs = [[count, genStep, compMode, [longs]],...] over the 4096 columns vs the section.
std::string diffRuns(const FullDataSource& s, const json& runs) {
    std::size_t i = 0;
    for (const json& r : runs) {
        const int  cnt = r.at(0).get<int>(), g = r.at(1).get<int>(), c = r.at(2).get<int>();
        const auto pts = gf::longs(r.at(3));
        for (int k = 0; k < cnt; ++k, ++i) {
            if (i >= FullDataSource::COLUMN_COUNT) return "runs cover more than 4096 columns";
            const auto where = "column " + std::to_string(i) + " (x=" + std::to_string(i / 64)
                             + ",z=" + std::to_string(i % 64) + "): ";
            if (s.columns[i] != pts)
                return where + "datapoints C++ " + gf::colStr(s.columns[i]) + " vs Java " + gf::colStr(pts);
            if (s.columnGenerationStep[i] != g)
                return where + "genStep C++ " + std::to_string(s.columnGenerationStep[i]) + " vs Java "
                     + std::to_string(g);
            if (s.columnWorldCompressionMode[i] != c)
                return where + "compMode C++ " + std::to_string(s.columnWorldCompressionMode[i]) + " vs Java "
                     + std::to_string(c);
        }
    }
    return i == FullDataSource::COLUMN_COUNT ? "" : "runs cover " + std::to_string(i) + " columns";
}

std::string posStr(std::uint64_t p) { return gf::hx(p); }

/// Compares the saved positions as a multiset (setOnly = false) or as a set. "" when equal.
std::string diffOrder(std::vector<std::uint64_t> cpp, std::vector<std::uint64_t> java, bool setOnly) {
    std::sort(cpp.begin(), cpp.end());
    std::sort(java.begin(), java.end());
    std::map<std::uint64_t, int> n;
    for (auto p : cpp) ++n[p];
    for (auto p : java) --n[p];
    std::string d;
    int         shown = 0;
    for (auto& [p, k] : n) {
        if (k == 0) continue;
        if (setOnly && std::binary_search(cpp.begin(), cpp.end(), p) && std::binary_search(java.begin(), java.end(), p))
            continue;
        if (shown++ < 8) d += " " + posStr(p) + (k > 0 ? " C+++" : " Java+") + std::to_string(std::abs(k));
    }
    if (d.empty()) return "";
    return std::string(setOnly ? "saved position SETS differ:" : "saved position multisets differ:") + d;
}

} // namespace

TEST_CASE("golden propagate: updater + propagator replay on MemoryLodStore") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "propagate");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    static const char* const kSlow[] = {
        "mid_applyToChildren_0",
        "mid_applyToChildren_1",
        "mid_applyToChildren_3",
        "unaligned_k4_2",
        "sparse_k4_0",
        "sparse_k4_2",
        "aligned_k4_4",
        "aligned_k4_5",
        "unaligned_k4_0",
        "rewrite_1",
        "rewrite_3",
    };
    const bool slowOn  = std::getenv("DH_GOLDEN_SLOW") != nullptr;
    int        skipped = 0;
    for (const GoldenCase& gc : s.cases) {
        const bool slow = gc.name.find("_k8_") != std::string::npos
                       || std::find(std::begin(kSlow), std::end(kSlow), gc.name) != std::end(kSlow);
        if (slow && !slowOn) {
            ++skipped;
            continue;
        }
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);

        MemoryLodStore store;
        ManualClock    clock;
        InlineExecutor exec;
        // The dumped level has sky light (ProviderOptions::maxSkyLight 15 is the default).
        // Fill/leaf comp mode is 1: the dump ran with worldCompression = VISUALLY_EQUAL (the Java config default).
        ProviderOptions po;
        po.update.worldCompression = WorldCompressionMode::VisuallyEqual;
        SourceProvider   provider(store, reg->registry(), clock, exec, CodecOptions{}, po);
        UpdatePropagator prop(provider, exec, 4);
        CHECK_EQ(prop.maxTaskCount(), gc.in.at("maxTasks").get<std::size_t>());

        std::vector<std::uint64_t> saved;
        provider.addUpdateListener([&](std::uint64_t p) { saved.push_back(p); });

        const json& phasesIn  = gc.in.at("phases");
        const json& phasesOut = gc.out.at("phases");
        REQUIRE_EQ(phasesIn.size(), phasesOut.size());
        for (std::size_t pi = 0; pi < phasesIn.size(); ++pi) {
            INFO("phase ", pi);
            saved.clear();
            for (const json& e : phasesIn[pi]) {
                auto src = gf::fromGoldenSourceFilled(e.at("source"), *reg);
                auto opt = [&](const char* k) {
                    return e.at(k).is_null() ? std::optional<bool>() : std::optional<bool>(e.at(k).get<bool>());
                };
                src->applyToParent   = opt("applyToParent");
                src->applyToChildren = opt("applyToChildren");
                src->regenerateLeaf  = opt("regenerateLeaf");
                auto r               = provider.updater().update(std::move(src));
                REQUIRE(r.has_value());
            }
            int passes = 0;
            for (; passes < 1000;) {
                prop.tick(BlockPos2D{0, 0});
                ++passes;
                auto up   = store.positionsToPropagateUp(0, 0, 1000);
                auto down = store.positionsToPropagateDown(0, 0, 1000);
                REQUIRE(up.has_value());
                REQUIRE(down.has_value());
                if (up->empty() && down->empty()) break;
            }
            const json& po = phasesOut[pi];
            ++st.checked;
            std::vector<std::uint64_t> jo;
            for (const json& p : po.at("updateOrder")) jo.push_back(hexU64(p));
            const bool setOnly = phasesIn[pi].size() > prop.maxTaskCount();
            CHECK_EQ(diffOrder(saved, jo, setOnly), std::string());
            if (setOnly && !diffOrder(saved, jo, false).empty())
                MESSAGE(
                    gc.name,
                    " phase ",
                    pi,
                    ": multiplicities differ (C++ ",
                    saved.size(),
                    " saves, Java ",
                    jo.size(),
                    "): ",
                    diffOrder(saved, jo, false)
                );
            if (passes != po.at("passes").get<int>())
                MESSAGE(gc.name, " phase ", pi, ": passes C++ ", passes, " vs Java ", po.at("passes").get<int>());
        }

        // ---- final stored sections
        auto all = store.allPositions();
        REQUIRE(all.has_value());
        std::vector<std::uint64_t> pos = *all;
        std::sort(pos.begin(), pos.end(), [](std::uint64_t a, std::uint64_t b) {
            const int da = sec::detail(a), db = sec::detail(b);
            if (da != db) return da < db;
            if (sec::x(a) != sec::x(b)) return sec::x(a) < sec::x(b);
            return sec::z(a) < sec::z(b);
        });
        const json& secs = gc.out.at("sections");
        ++st.checked;
        REQUIRE_EQ(pos.size(), secs.size());
        for (std::size_t i = 0; i < secs.size(); ++i) {
            const json& js = secs[i];
            INFO("section ", js.at("pos").get<std::string>());
            ++st.checked;
            REQUIRE_EQ(posStr(pos[i]), posStr(hexU64(js.at("pos"))));
            auto rec = store.get(pos[i], StorePart::Meta);
            REQUIRE((rec.has_value() && rec->has_value()));
            const StoreMeta& m = (*rec)->meta;
            ++st.checked;
            CHECK_EQ(m.hasFlag(StoreFlag::ApplyToParent), js.at("applyToParent").get<bool>());
            ++st.checked;
            CHECK_EQ(m.hasFlag(StoreFlag::ApplyToChildren), js.at("applyToChildren").get<bool>());
            ++st.checked;
            CHECK_EQ(m.hasFlag(StoreFlag::Regenerate), js.at("regenerate").get<bool>());
            auto fs = provider.loadFresh(pos[i]);
            REQUIRE(fs);
            const json& d = js.at("data");
            ++st.checked;
            CHECK_EQ(posStr(fs->pos), posStr(hexU64(d.at("pos"))));
            ++st.checked;
            CHECK_EQ(fs->isEmpty, d.at("isEmpty").get<bool>());
            ++st.checked;
            CHECK_EQ(gf::diffPalette(fs->mapping, d.at("palette")), std::string());
            ++st.checked;
            CHECK_EQ(diffRuns(*fs, d.at("runs")), std::string());
        }
    }
    std::fprintf(stderr, "[golden] propagate skipped_for_speed=%d (DH_GOLDEN_SLOW=1 runs them)\n", skipped);
    gv::report("propagate", st);
}
