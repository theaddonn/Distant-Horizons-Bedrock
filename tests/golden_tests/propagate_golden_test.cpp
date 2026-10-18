// Golden replay for tests/golden/propagate.gv.json (WP-D3: SourceProvider + SourceUpdater + UpdatePropagator).
//
// Per case: MemoryLodStore + golden registry + ManualClock + InlineExecutor, propagator threadCount 4 (maxTasks 40),
// target (0,0). Each phase writes its entries through SourceUpdater::update (an update listener records the saved
// positions), then runs tick({0,0}) until positionsToPropagateUp/Down are both empty.
//
// Checked out keys: phases[i].updateOrder (compared as a sequence), phases[i].passes (see below), sections (set of
//   positions, then per section pos / applyToParent / applyToChildren / regenerate / data.pos / data.isEmpty /
//   data.palette / data.runs expanded over all 4096 columns).
// Null mapping: the C++ store keeps flags as bits (null == false, see ILodStore.h StoreFlag). The dumped out.sections
//   never contain a null flag (all are true/false), so the question is moot on the output side; null on the input
//   side (applyToChildren / regenerateLeaf) maps to std::nullopt ("don't touch"), as in Java.
// passes: Java counted loop iterations of runParentUpdates+runChildUpdates until both queues were empty; C++ counts
//   tick() calls that were executed (the check for empty happens after each tick, like Java's do-while).
// SKIPPED_KEYS: none (`in.maxTasks` is only asserted equal to maxTaskCount()).
#include <doctest.h>

#include <algorithm>
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

/// First difference between two position sequences, "" when equal.
std::string diffOrder(const std::vector<std::uint64_t>& cpp, const std::vector<std::uint64_t>& java) {
    if (cpp == java) return "";
    auto a = cpp, b = java;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    std::string extra;
    if (a != b) { // multiset difference: positions whose update count differs
        std::map<std::uint64_t, int> n;
        for (auto p : a) ++n[p];
        for (auto p : b) --n[p];
        for (auto& [p, d] : n)
            if (d != 0) extra += " " + posStr(p) + (d > 0 ? " C+++" : " Java+") + std::to_string(std::abs(d));
        extra = " multiset diff:" + extra;
    }
    std::string s = std::string(a == b ? "[same positions, different order] " : "[DIFFERENT position sets] ")
                  + "updateOrder differs" + extra + " (C++ " + std::to_string(cpp.size()) + " entries, Java "
                  + std::to_string(java.size()) + "): ";
    for (std::size_t i = 0; i < std::max(cpp.size(), java.size()); ++i) {
        const std::string a = i < cpp.size() ? posStr(cpp[i]) : "-", b = i < java.size() ? posStr(java[i]) : "-";
        if (a != b) return s + "first at [" + std::to_string(i) + "] C++ " + a + " vs Java " + b;
    }
    return s;
}

} // namespace

TEST_CASE("golden propagate: updater + propagator replay on MemoryLodStore") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "propagate");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
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
            CHECK_EQ(diffOrder(saved, jo), std::string());
            ++st.checked;
            CHECK_EQ(passes, po.at("passes").get<int>());
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
    gv::report("propagate", st);
}
