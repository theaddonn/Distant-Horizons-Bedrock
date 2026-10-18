#pragma once

// Reusable ILodStore conformance suite (WP-D2). Every backend (MemoryLodStore
// now, the LevelDB store in the Bedrock glue later) instantiates it:
//
//   TEST_CASE("MemoryLodStore conformance") {
//       auto backing = std::make_shared<dh::MemoryLodStore>();
//       runStoreConformance([&](bool reopen) -> std::shared_ptr<dh::ILodStore> {
//           if (!reopen) backing = std::make_shared<dh::MemoryLodStore>();
//           return backing;
//       });
//   }
//
// MakeStore contract: `make(false)` returns a new EMPTY store (fresh backing,
// e.g. a new temp dir); `make(true)` returns a store over the backing of the
// last `make(false)` call, as if the process had restarted (a LevelDB backend
// closes the previous handle and reopens the same dir; the caller drops its
// previous shared_ptr first).
//
// Thread-safety check: run under ThreadSanitizer to catch data races, e.g.
//   cd tests && xmake f -P . -p linux -m debug --cc=clang-22 --cxx=clang++-22 \
//       --ld=clang++-22 --cxflags="-fsanitize=thread" --ldflags="-fsanitize=thread" -y
//   (ASan and TSan cannot be combined: drop the address sanitizer flags from
//   tests/xmake.lua locally for a TSan run), then `xmake run -P . dh_core_tests -tc="*conformance*"`.
//
// Hand-computed ordering fixture (all coordinates are section detail levels;
// min corner block = x << detail, z << detail; query block = (0,0)):
//   A = (6, 0, 0)  corner (   0,   0) dist   0  pos 6
//   F = (7, 0, 0)  corner (   0,   0) dist   0  pos 7           (tie with A -> A first)
//   B = (6, 1, 0)  corner (  64,   0) dist  64
//   D = (6,-1, 0)  corner ( -64,   0) dist  64  (28-bit x field, pos < C)
//   C = (6, 0, 1)  corner (   0,  64) dist  64  (z in the high bits, pos largest)
//   E = (7, 1, 0)  corner ( 128,   0) dist 128
//   order from (0,0):     A F B D C E
//   order from (100,100): E(128) B(136) C(136) A(200) F(200) D(264)

#include <doctest.h>

#include "dh/platform/ILodStore.h"
#include "dh/pos/SectionPos.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dh::conformance {

inline std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

inline StoreRecord makeRecord(std::uint64_t pos, int seed, std::int64_t lastModified = 0, std::uint8_t flags = 0) {
    StoreRecord r;
    r.pos  = pos;
    r.data = bytes({seed, seed + 1, seed + 2});
    for (int i = 0; i < 4; i++) r.adjacent[static_cast<std::size_t>(i)] = bytes({seed + 10 + i});
    r.genSteps                = bytes({seed + 20, seed + 21});
    r.compressionModes        = bytes({seed + 30});
    r.mapping                 = bytes({seed + 40, seed + 41, seed + 42, seed + 43});
    r.meta.dataFormatVersion  = 3;
    r.meta.compressionMode    = 2;
    r.meta.flags              = flags;
    r.meta.dataChecksum       = 0xDEADBEEFu + static_cast<std::uint32_t>(seed);
    r.meta.createdMillis      = 5;
    r.meta.lastModifiedMillis = lastModified;
    return r;
}

inline bool sameBlobs(const StoreRecord& a, const StoreRecord& b) {
    return a.data == b.data && a.adjacent == b.adjacent && a.genSteps == b.genSteps
        && a.compressionModes == b.compressionModes && a.mapping == b.mapping;
}

inline bool sameMeta(const StoreMeta& a, const StoreMeta& b) {
    return a.dataFormatVersion == b.dataFormatVersion && a.compressionMode == b.compressionMode && a.flags == b.flags
        && a.dataChecksum == b.dataChecksum && a.createdMillis == b.createdMillis
        && a.lastModifiedMillis == b.lastModifiedMillis;
}

using PosList = std::vector<std::uint64_t>;

/// Creates the A..F fixture, all with `flags`.
struct OrderFixture {
    std::uint64_t A = sec::encode(6, 0, 0);
    std::uint64_t F = sec::encode(7, 0, 0);
    std::uint64_t B = sec::encode(6, 1, 0);
    std::uint64_t D = sec::encode(6, -1, 0);
    std::uint64_t C = sec::encode(6, 0, 1);
    std::uint64_t E = sec::encode(7, 1, 0);

    std::vector<std::uint64_t> all() const { return {A, F, B, D, C, E}; }
    void                       fill(ILodStore& s, std::uint8_t flags) const {
        int seed = 1;
        for (std::uint64_t p : all()) {
            REQUIRE(s.put(makeRecord(p, seed++, 100, flags)).has_value());
        }
    }
};

} // namespace dh::conformance

template <class MakeStore>
void runStoreConformance(MakeStore&& make) {
    using namespace dh;
    using namespace dh::conformance;

    const std::uint64_t P1 = sec::encode(6, 3, -4);
    const std::uint64_t P2 = sec::encode(8, -7, 9);

    SUBCASE("get on missing position returns nullopt, exists false") {
        auto s = make(false);
        auto r = s->get(P1);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->has_value());
        auto e = s->exists(P1);
        REQUIRE(e.has_value());
        CHECK_FALSE(*e);
    }

    SUBCASE("put/get all parts round trip") {
        auto        s   = make(false);
        StoreRecord rec = makeRecord(P1, 7, 1234, flagBit(StoreFlag::Regenerate));
        REQUIRE(s->put(rec).has_value());
        auto r = s->get(P1);
        REQUIRE(r.has_value());
        REQUIRE(r->has_value());
        CHECK((*r)->pos == P1);
        CHECK(sameBlobs(**r, rec));
        CHECK(sameMeta((*r)->meta, rec.meta));
        auto e = s->exists(P1);
        REQUIRE(e.has_value());
        CHECK(*e);
    }

    SUBCASE("get returns only the requested parts") {
        auto        s   = make(false);
        StoreRecord rec = makeRecord(P1, 7, 99, flagBit(StoreFlag::ApplyToParent));
        REQUIRE(s->put(rec).has_value());

        struct Case {
            StorePart parts;
        };
        const StorePart singles[] = {
            StorePart::Data,
            StorePart::Adjacent,
            StorePart::GenSteps,
            StorePart::CompressionModes,
            StorePart::Mapping,
            StorePart::Meta
        };
        // Every subset of the six bits.
        for (unsigned mask = 0; mask < 64; mask++) {
            StorePart parts = StorePart::None;
            for (unsigned i = 0; i < 6; i++) {
                if (mask & (1u << i)) parts = parts | singles[i];
            }
            auto r = s->get(P1, parts);
            REQUIRE(r.has_value());
            REQUIRE(r->has_value());
            const StoreRecord& g = **r;
            CHECK(g.pos == P1);
            CHECK((g.data == rec.data) == has(parts, StorePart::Data));
            CHECK((g.adjacent == rec.adjacent) == has(parts, StorePart::Adjacent));
            CHECK((g.genSteps == rec.genSteps) == has(parts, StorePart::GenSteps));
            CHECK((g.compressionModes == rec.compressionModes) == has(parts, StorePart::CompressionModes));
            CHECK((g.mapping == rec.mapping) == has(parts, StorePart::Mapping));
            CHECK(sameMeta(g.meta, rec.meta) == has(parts, StorePart::Meta));
            if (!has(parts, StorePart::Data)) CHECK(g.data.empty());
            if (!has(parts, StorePart::Adjacent)) {
                for (const auto& strip : g.adjacent) CHECK(strip.empty());
            }
            if (!has(parts, StorePart::GenSteps)) CHECK(g.genSteps.empty());
            if (!has(parts, StorePart::CompressionModes)) CHECK(g.compressionModes.empty());
            if (!has(parts, StorePart::Mapping)) CHECK(g.mapping.empty());
        }
    }

    SUBCASE("partial put leaves other parts untouched") {
        auto        s = make(false);
        StoreRecord a = makeRecord(P1, 1, 10);
        StoreRecord b = makeRecord(P1, 50, 20, flagBit(StoreFlag::Regenerate));
        REQUIRE(s->put(a).has_value());
        REQUIRE(s->put(b, StorePart::Data | StorePart::GenSteps).has_value());
        auto r = s->get(P1);
        REQUIRE(r.has_value());
        REQUIRE(r->has_value());
        const StoreRecord& g = **r;
        CHECK(g.data == b.data);
        CHECK(g.genSteps == b.genSteps);
        CHECK(g.adjacent == a.adjacent);
        CHECK(g.compressionModes == a.compressionModes);
        CHECK(g.mapping == a.mapping);
        CHECK(sameMeta(g.meta, a.meta)); // meta not in the part set

        REQUIRE(s->put(b, StorePart::Meta).has_value());
        auto r2 = s->get(P1);
        REQUIRE(r2.has_value());
        CHECK(sameMeta((*r2)->meta, b.meta));
        CHECK((*r2)->data == b.data);
        // The Meta-only put made the record Regenerate-flagged.
        auto regen = s->positionsToRegen(0, 0, -1, 10);
        REQUIRE(regen.has_value());
        CHECK(*regen == PosList{P1});
    }

    SUBCASE("put overwrites an existing record") {
        auto s = make(false);
        REQUIRE(s->put(makeRecord(P1, 1, 10, flagBit(StoreFlag::ApplyToParent))).has_value());
        StoreRecord second = makeRecord(P1, 90, 20); // no flags
        REQUIRE(s->put(second).has_value());
        auto r = s->get(P1);
        REQUIRE(r.has_value());
        REQUIRE(r->has_value());
        CHECK(sameBlobs(**r, second));
        CHECK(sameMeta((*r)->meta, second.meta));
        auto up = s->positionsToPropagateUp(0, 0, 10);
        REQUIRE(up.has_value());
        CHECK(up->empty()); // old flag must not linger in the index
        auto all = s->allPositions();
        REQUIRE(all.has_value());
        CHECK(all->size() == 1);
    }

    SUBCASE("erase removes the record, is idempotent, and drops flags") {
        auto s = make(false);
        REQUIRE(s->put(makeRecord(P1, 1, 10, flagBit(StoreFlag::ApplyToParent) | flagBit(StoreFlag::Regenerate)))
                    .has_value());
        REQUIRE(s->put(makeRecord(P2, 2, 10)).has_value());
        REQUIRE(s->erase(P1).has_value());
        auto e = s->exists(P1);
        REQUIRE(e.has_value());
        CHECK_FALSE(*e);
        auto g = s->get(P1);
        REQUIRE(g.has_value());
        CHECK_FALSE(g->has_value());
        auto e2 = s->exists(P2);
        REQUIRE(e2.has_value());
        CHECK(*e2);
        CHECK(s->erase(P1).has_value()); // absent -> success
        auto up = s->positionsToPropagateUp(0, 0, 10);
        REQUIRE(up.has_value());
        CHECK(up->empty());
        auto rc = s->regenCount(0, 0, -1);
        REQUIRE(rc.has_value());
        CHECK(*rc == 0);
        auto t = s->timestamp(P1);
        REQUIRE(t.has_value());
        CHECK_FALSE(t->has_value());
    }

    SUBCASE("setFlag on a missing record is NotFound") {
        auto s = make(false);
        auto r = s->setFlag(P1, StoreFlag::Regenerate, true);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::NotFound);
    }

    SUBCASE("setFlag sets and clears each flag independently and never touches blobs") {
        auto        s   = make(false);
        StoreRecord rec = makeRecord(P1, 4, 77);
        REQUIRE(s->put(rec).has_value());

        const StoreFlag flags[] = {StoreFlag::ApplyToParent, StoreFlag::ApplyToChildren, StoreFlag::Regenerate};
        for (StoreFlag f : flags) {
            REQUIRE(s->setFlag(P1, f, true).has_value());
            auto r = s->get(P1, StorePart::Meta);
            REQUIRE(r.has_value());
            CHECK((*r)->meta.hasFlag(f));
        }
        REQUIRE(s->setFlag(P1, StoreFlag::ApplyToChildren, false).has_value());
        {
            auto r = s->get(P1);
            REQUIRE(r.has_value());
            const StoreRecord& g = **r;
            CHECK(g.meta.hasFlag(StoreFlag::ApplyToParent));
            CHECK_FALSE(g.meta.hasFlag(StoreFlag::ApplyToChildren));
            CHECK(g.meta.hasFlag(StoreFlag::Regenerate));
            CHECK(sameBlobs(g, rec));
            CHECK(g.meta.lastModifiedMillis == 77);
            CHECK(g.meta.dataChecksum == rec.meta.dataChecksum);
        }
        // Each index reflects the flag state.
        auto up = s->positionsToPropagateUp(0, 0, 10);
        auto dn = s->positionsToPropagateDown(0, 0, 10);
        auto rg = s->positionsToRegen(0, 0, -1, 10);
        REQUIRE(up.has_value());
        REQUIRE(dn.has_value());
        REQUIRE(rg.has_value());
        CHECK(*up == PosList{P1});
        CHECK(dn->empty());
        CHECK(*rg == PosList{P1});
        // Setting an already-set flag / clearing an already-clear flag is fine.
        CHECK(s->setFlag(P1, StoreFlag::ApplyToParent, true).has_value());
        CHECK(s->setFlag(P1, StoreFlag::ApplyToChildren, false).has_value());
    }

    SUBCASE("positionsToPropagateUp: Manhattan from min corner, ties by pos") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, flagBit(StoreFlag::ApplyToParent));
        auto r = s->positionsToPropagateUp(0, 0, 100);
        REQUIRE(r.has_value());
        CHECK(*r == PosList{f.A, f.F, f.B, f.D, f.C, f.E});
        auto r2 = s->positionsToPropagateUp(100, 100, 100);
        REQUIRE(r2.has_value());
        CHECK(*r2 == PosList{f.E, f.B, f.C, f.A, f.F, f.D});
        // The other two flags are not set.
        auto dn = s->positionsToPropagateDown(0, 0, 100);
        auto rg = s->positionsToRegen(0, 0, -1, 100);
        REQUIRE(dn.has_value());
        REQUIRE(rg.has_value());
        CHECK(dn->empty());
        CHECK(rg->empty());
    }

    SUBCASE("positionsToPropagateDown: same ordering, own flag") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, flagBit(StoreFlag::ApplyToChildren));
        auto r = s->positionsToPropagateDown(0, 0, 100);
        REQUIRE(r.has_value());
        CHECK(*r == PosList{f.A, f.F, f.B, f.D, f.C, f.E});
        auto up = s->positionsToPropagateUp(0, 0, 100);
        REQUIRE(up.has_value());
        CHECK(up->empty());
    }

    SUBCASE("only flagged records are returned") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, 0);
        REQUIRE(s->setFlag(f.C, StoreFlag::ApplyToParent, true).has_value());
        REQUIRE(s->setFlag(f.A, StoreFlag::ApplyToParent, true).has_value());
        REQUIRE(s->setFlag(f.E, StoreFlag::ApplyToChildren, true).has_value());
        auto up = s->positionsToPropagateUp(0, 0, 100);
        auto dn = s->positionsToPropagateDown(0, 0, 100);
        REQUIRE(up.has_value());
        REQUIRE(dn.has_value());
        CHECK(*up == PosList{f.A, f.C});
        CHECK(*dn == PosList{f.E});
        REQUIRE(s->setFlag(f.A, StoreFlag::ApplyToParent, false).has_value());
        auto up2 = s->positionsToPropagateUp(0, 0, 100);
        REQUIRE(up2.has_value());
        CHECK(*up2 == PosList{f.C});
    }

    SUBCASE("maxCount truncates the nearest-first list") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, flagBit(StoreFlag::ApplyToParent) | flagBit(StoreFlag::Regenerate));
        auto r0 = s->positionsToPropagateUp(0, 0, 0);
        REQUIRE(r0.has_value());
        CHECK(r0->empty());
        auto r1 = s->positionsToPropagateUp(0, 0, 1);
        REQUIRE(r1.has_value());
        CHECK(*r1 == PosList{f.A});
        auto r4 = s->positionsToPropagateUp(0, 0, 4);
        REQUIRE(r4.has_value());
        CHECK(*r4 == PosList{f.A, f.F, f.B, f.D}); // tie group cut at its pos order
        auto rr = s->positionsToRegen(100, 100, -1, 3);
        REQUIRE(rr.has_value());
        CHECK(*rr == PosList{f.E, f.B, f.C});
    }

    SUBCASE("positionsToRegen honours maxBlockDistance (inclusive), negative = everything") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, flagBit(StoreFlag::Regenerate));
        auto d0 = s->positionsToRegen(0, 0, 0, 100);
        REQUIRE(d0.has_value());
        CHECK(*d0 == PosList{f.A, f.F});
        auto d63 = s->positionsToRegen(0, 0, 63, 100);
        REQUIRE(d63.has_value());
        CHECK(*d63 == PosList{f.A, f.F});
        auto d64 = s->positionsToRegen(0, 0, 64, 100);
        REQUIRE(d64.has_value());
        CHECK(*d64 == PosList{f.A, f.F, f.B, f.D, f.C});
        auto d64n = s->positionsToRegen(0, 0, 64, 3);
        REQUIRE(d64n.has_value());
        CHECK(*d64n == PosList{f.A, f.F, f.B});
        auto neg = s->positionsToRegen(0, 0, -1, 100);
        REQUIRE(neg.has_value());
        CHECK(*neg == PosList{f.A, f.F, f.B, f.D, f.C, f.E});
        auto far = s->positionsToRegen(100, 100, 135, 100);
        REQUIRE(far.has_value());
        CHECK(*far == PosList{f.E}); // 128 <= 135 < 136
    }

    SUBCASE("regenCount counts Regenerate records within distance") {
        auto         s = make(false);
        OrderFixture f;
        f.fill(*s, flagBit(StoreFlag::Regenerate));
        REQUIRE(s->setFlag(f.E, StoreFlag::Regenerate, false).has_value());
        auto all = s->regenCount(0, 0, -1);
        auto d0  = s->regenCount(0, 0, 0);
        auto d64 = s->regenCount(0, 0, 64);
        auto d63 = s->regenCount(0, 0, 63);
        REQUIRE(all.has_value());
        REQUIRE(d0.has_value());
        REQUIRE(d64.has_value());
        REQUIRE(d63.has_value());
        CHECK(*all == 5);
        CHECK(*d0 == 2);
        CHECK(*d64 == 5);
        CHECK(*d63 == 2);
    }

    SUBCASE("timestamp(pos): present and missing") {
        auto s = make(false);
        REQUIRE(s->put(makeRecord(P1, 1, 4242)).has_value());
        auto t = s->timestamp(P1);
        REQUIRE(t.has_value());
        REQUIRE(t->has_value());
        CHECK(**t == 4242);
        auto m = s->timestamp(P2);
        REQUIRE(m.has_value());
        CHECK_FALSE(m->has_value());
    }

    SUBCASE("timestamps(range): bounds inclusive, detail filtered, sorted by pos") {
        auto s = make(false);
        for (int x = -1; x <= 4; x++) {
            for (int z = -1; z <= 1; z++) {
                REQUIRE(s->put(makeRecord(sec::encode(6, x, z), 1, 1000 + x * 10 + z)).has_value());
            }
        }
        // Same coordinates at another detail level must not appear.
        REQUIRE(s->put(makeRecord(sec::encode(7, 2, 0), 1, 5)).has_value());

        auto r = s->timestamps(6, 1, 0, 3, 0);
        REQUIRE(r.has_value());
        REQUIRE(r->size() == 3); // x = 1,2,3 at z = 0
        CHECK((*r)[0] == std::make_pair(sec::encode(6, 1, 0), std::int64_t{1010}));
        CHECK((*r)[1] == std::make_pair(sec::encode(6, 2, 0), std::int64_t{1020}));
        CHECK((*r)[2] == std::make_pair(sec::encode(6, 3, 0), std::int64_t{1030}));

        // Single cell: start == end is inclusive.
        auto one = s->timestamps(6, 2, 1, 2, 1);
        REQUIRE(one.has_value());
        REQUIRE(one->size() == 1);
        CHECK((*one)[0].first == sec::encode(6, 2, 1));
        CHECK((*one)[0].second == 1021);

        // Negative coordinates.
        auto neg = s->timestamps(6, -1, -1, 0, 0);
        REQUIRE(neg.has_value());
        CHECK(neg->size() == 4);
        CHECK(std::is_sorted(neg->begin(), neg->end(), [](const auto& a, const auto& b) { return a.first < b.first; }));

        // Other detail level and empty range.
        auto d7 = s->timestamps(7, -10, -10, 10, 10);
        REQUIRE(d7.has_value());
        REQUIRE(d7->size() == 1);
        CHECK((*d7)[0].first == sec::encode(7, 2, 0));
        auto none = s->timestamps(6, 10, 10, 20, 20);
        REQUIRE(none.has_value());
        CHECK(none->empty());
        auto inverted = s->timestamps(6, 3, 0, 1, 0);
        REQUIRE(inverted.has_value());
        CHECK(inverted->empty());
    }

    SUBCASE("allPositions is sorted ascending and deduplicated") {
        auto                       s  = make(false);
        std::vector<std::uint64_t> in = {
            sec::encode(8, 5, 5),
            sec::encode(6, 0, 0),
            sec::encode(6, -3, 2),
            sec::encode(7, 1, -1),
            sec::encode(6, 0, 1)
        };
        for (std::uint64_t p : in) REQUIRE(s->put(makeRecord(p, 1)).has_value());
        REQUIRE(s->put(makeRecord(in[0], 2)).has_value()); // overwrite, no duplicate
        auto all = s->allPositions();
        REQUIRE(all.has_value());
        std::vector<std::uint64_t> expected = in;
        std::sort(expected.begin(), expected.end());
        CHECK(*all == expected);
    }

    SUBCASE("flush succeeds") {
        auto s = make(false);
        REQUIRE(s->put(makeRecord(P1, 1)).has_value());
        CHECK(s->flush().has_value());
    }

    SUBCASE("reopen preserves records, parts, flags and indexes") {
        OrderFixture f;
        StoreRecord  rec = makeRecord(P1, 9, 31337, flagBit(StoreFlag::ApplyToChildren));
        {
            auto s = make(false);
            f.fill(*s, flagBit(StoreFlag::Regenerate));
            REQUIRE(s->put(rec).has_value());
            REQUIRE(s->erase(f.E).has_value());
            REQUIRE(s->flush().has_value());
        }
        auto s = make(true);
        auto r = s->get(P1);
        REQUIRE(r.has_value());
        REQUIRE(r->has_value());
        CHECK(sameBlobs(**r, rec));
        CHECK(sameMeta((*r)->meta, rec.meta));
        auto e = s->exists(f.E);
        REQUIRE(e.has_value());
        CHECK_FALSE(*e);
        auto rg = s->positionsToRegen(0, 0, -1, 100);
        REQUIRE(rg.has_value());
        CHECK(*rg == PosList{f.A, f.F, f.B, f.D, f.C});
        auto dn = s->positionsToPropagateDown(0, 0, 100);
        REQUIRE(dn.has_value());
        CHECK(*dn == PosList{P1});
        auto all = s->allPositions();
        REQUIRE(all.has_value());
        CHECK(all->size() == 6);
        auto t = s->timestamp(P1);
        REQUIRE(t.has_value());
        REQUIRE(t->has_value());
        CHECK(**t == 31337);
    }

    SUBCASE("concurrent put/get/setFlag from 8 threads is atomic and race free") {
        std::shared_ptr<ILodStore> s        = make(false);
        constexpr int              kThreads = 8;
        constexpr int              kIters   = 200;
        // Every record is written with all blobs == value and lastModified == value, so a
        // reader that sees a mix of two writes (torn put) detects it.
        const std::uint64_t shared = sec::encode(6, 100, 100);
        auto                fill   = [&](int value) {
            StoreRecord r = makeRecord(shared, 0, value);
            auto        b = bytes({value, value, value});
            r.data        = b;
            for (auto& strip : r.adjacent) strip = b;
            r.genSteps         = b;
            r.compressionModes = b;
            r.mapping          = b;
            return r;
        };
        REQUIRE(s->put(fill(1)).has_value());

        std::atomic<int>         failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; t++) {
            threads.emplace_back([&, t] {
                const std::uint64_t own = sec::encode(6, t, 0);
                for (int i = 1; i <= kIters; i++) {
                    if (t % 2 == 0) {
                        if (!s->put(fill(t * 1000 + i)).has_value()) failures++;
                        if (!s->put(makeRecord(own, i, i)).has_value()) failures++;
                        if (!s->setFlag(own, StoreFlag::Regenerate, i % 2 == 0).has_value()) failures++;
                    } else {
                        auto r = s->get(shared);
                        if (!r.has_value() || !r->has_value()) {
                            failures++;
                        } else {
                            const StoreRecord& g = **r;
                            const auto         v = static_cast<int>(g.meta.lastModifiedMillis);
                            const auto         b = bytes({v, v, v});
                            if (g.data != b || g.genSteps != b || g.compressionModes != b || g.mapping != b
                                || g.adjacent[0] != b || g.adjacent[3] != b) {
                                failures++;
                            }
                        }
                        if (!s->positionsToRegen(0, 0, -1, 4).has_value()) failures++;
                        if (!s->allPositions().has_value()) failures++;
                        if (!s->timestamps(6, 0, 0, 8, 0).has_value()) failures++;
                        if (!s->exists(own).has_value()) failures++;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(failures.load() == 0);
        auto all = s->allPositions();
        REQUIRE(all.has_value());
        CHECK(all->size() == 1 + kThreads / 2);
    }
}
