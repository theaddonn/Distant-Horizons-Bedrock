#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/fulldata/IdMap.h"

using namespace dh;

namespace {

/// java.util.Random (48-bit LCG), used to port DataSourceRepoTests' seeded data exactly.
struct JavaRandom {
    std::uint64_t seed;
    explicit JavaRandom(std::int64_t s) : seed((static_cast<std::uint64_t>(s) ^ 0x5DEECE66Dull) & ((1ull << 48) - 1)) {}
    int next(int bits) {
        seed = (seed * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1);
        return static_cast<int>(static_cast<std::int32_t>(static_cast<std::int64_t>(seed) >> (48 - bits)));
    }
    int nextInt() { return next(32); }
};

struct Fixture {
    BiomeInfo b0{"plains"}, b1{"desert"};
    BlockInfo k0{"stone"}, k1{"dirt"}, k2{"air"};
};

} // namespace

TEST_CASE("fdp: layout offsets") {
    const std::uint64_t d = fdp::encode(0x12345, 0xABC, 0xDEF, 0x7, 0x9);
    CHECK((d & 0xFFFFFFFFull) == 0x12345);
    CHECK(((d >> 32) & 0xFFF) == 0xABC);
    CHECK(((d >> 44) & 0xFFF) == 0xDEF);
    CHECK(((d >> 56) & 0xF) == 0x9);
    CHECK(((d >> 60) & 0xF) == 0x7);
    CHECK(fdp::EMPTY_DATA_POINT == 0);
    CHECK(fdp::isEmpty(0));
    CHECK_FALSE(fdp::isEmpty(d));
}

TEST_CASE("fdp: round trip fuzz") {
    std::mt19937_64 rng(42);
    for (int i = 0; i < 100000; ++i) {
        int id = static_cast<int>(rng() & 0x7FFFFFFF);
        int h = static_cast<int>(rng() & 0xFFF);
        int y = static_cast<int>(rng() & 0xFFF);
        auto bl = static_cast<std::uint8_t>(rng() & 0xF);
        auto sl = static_cast<std::uint8_t>(rng() & 0xF);
        if (i < 16) { // max / min values
            id = (i & 1) ? 0x7FFFFFFF : 0;
            h = (i & 2) ? 0xFFF : 0;
            y = (i & 4) ? 0xFFF : 0;
            bl = (i & 8) ? 15 : 0;
            sl = (i & 8) ? 0 : 15;
        }
        const std::uint64_t d = fdp::encode(id, h, y, bl, sl);
        CHECK(fdp::id(d) == id);
        CHECK(fdp::height(d) == h);
        CHECK(fdp::minY(d) == y);
        CHECK(fdp::blockLight(d) == bl);
        CHECK(fdp::skyLight(d) == sl);

        // setters touch exactly one field
        const int id2 = static_cast<int>(rng() & 0x7FFFFFFF);
        const int h2 = static_cast<int>(rng() & 0xFFF);
        const int y2 = static_cast<int>(rng() & 0xFFF);
        const auto bl2 = static_cast<std::uint8_t>(rng() & 0xF);
        const auto sl2 = static_cast<int>(rng() & 0xF);
        const std::uint64_t s1 = fdp::setId(d, id2);
        CHECK(fdp::id(s1) == id2);
        CHECK(fdp::encode(id2, h, y, bl, sl) == s1);
        CHECK(fdp::encode(id, h2, y, bl, sl) == fdp::setHeight(d, h2));
        CHECK(fdp::encode(id, h, y2, bl, sl) == fdp::setMinY(d, y2));
        CHECK(fdp::encode(id, h, y, bl2, sl) == fdp::setBlockLight(d, bl2));
        CHECK(fdp::encode(id, h, y, bl, static_cast<std::uint8_t>(sl2)) == fdp::setSkyLight(d, sl2));
    }
}

TEST_CASE("fdp: Java quirks are preserved") {
    // encode ORs the light bytes unmasked: skyLight 0x1F bleeds into the blockLight nibble.
    const std::uint64_t bleed = fdp::encode(1, 1, 1, 0, 0x1F);
    CHECK(fdp::skyLight(bleed) == 0xF);
    CHECK(fdp::blockLight(bleed) == 0x1);

    // a negative byte (as uint8 0xFF) for blockLight: only the low nibble survives the <<60.
    const std::uint64_t neg = fdp::encode(0, 1, 0, static_cast<std::uint8_t>(static_cast<std::int8_t>(-1)), 0);
    CHECK(fdp::blockLight(neg) == 0xF);
    CHECK((neg >> 60) == 0xF);
    CHECK(fdp::setBlockLight(0, 0xFF) == 0xF000000000000000ull);

    // setSkyLight(15+) bleeds upward, setBlockLight is not masked either
    CHECK(fdp::blockLight(fdp::setSkyLight(0, 0x10)) == 1);

    // ID_MASK is 31 bits: bit 31 of the id is dropped by encode and survives setId's clear.
    CHECK(fdp::id(fdp::encode(static_cast<int>(0x80000001u), 1, 0, 0, 0)) == 1);
    CHECK(fdp::id(fdp::encode(0x7FFFFFFF, 1, 0, 0, 0)) == 0x7FFFFFFF);

    // setHeight / setMinY do not mask their input (height 0x1000 bleeds into the minY field)
    CHECK(fdp::minY(fdp::setHeight(0, 0x1000)) == 1);
    CHECK(fdp::height(fdp::setHeight(0, 0x1000)) == 0);

    // encode masks height/minY
    CHECK(fdp::height(fdp::encode(0, 0x1005, 0, 0, 0)) == 5);
    CHECK(fdp::minY(fdp::encode(0, 0, 0x1005, 0, 0)) == 5);
}

TEST_CASE("fdp: remapId") {
    const int table[] = {5, 9, 2};
    const std::uint64_t d = fdp::encode(1, 7, 8, 3, 4);
    const std::uint64_t r = fdp::remapId(d, table);
    CHECK(fdp::id(r) == 9);
    CHECK(fdp::encode(9, 7, 8, 3, 4) == r);
}

TEST_CASE("IdMap: add, lookup, dedupe") {
    Fixture f;
    IdMap m(77);
    CHECK(m.empty());
    CHECK(m.maxValidId() == -1);
    CHECK(m.block(0) == nullptr);
    CHECK(m.addIfAbsent(&f.b0, &f.k0) == 0);
    CHECK(m.addIfAbsent(&f.b0, &f.k1) == 1);
    CHECK(m.addIfAbsent(&f.b1, &f.k0) == 2);
    CHECK(m.addIfAbsent(&f.b0, &f.k0) == 0);
    CHECK(m.addIfAbsent(&f.b1, &f.k0) == 2);
    CHECK(m.size() == 3);
    CHECK(m.maxValidId() == 2);
    CHECK(m.block(1) == &f.k1);
    CHECK(m.biome(1) == &f.b0);
    CHECK(m.biome(2) == &f.b1);
    CHECK(m.block(3) == nullptr);
    CHECK(m.block(-1) == nullptr);
    CHECK(m.sectionPos == 77);
    m.clear();
    CHECK(m.empty());
    CHECK(m.sectionPos == 77);
    m.clear(5);
    CHECK(m.sectionPos == 5);
    CHECK(m.addIfAbsent(&f.b1, &f.k0) == 0);
}

TEST_CASE("IdMap: mergeAndRemap") {
    Fixture f;
    IdMap a, b;
    a.addIfAbsent(&f.b0, &f.k0);
    a.addIfAbsent(&f.b0, &f.k1);
    b.addIfAbsent(&f.b0, &f.k1); // exists in a as id 1
    b.addIfAbsent(&f.b1, &f.k2); // new
    b.addIfAbsent(&f.b0, &f.k0); // exists in a as id 0
    const std::vector<int> remap = a.mergeAndRemap(b);
    CHECK(remap == std::vector<int>{1, 2, 0});
    CHECK(a.size() == 3);
    CHECK(a.block(2) == &f.k2);
    // merging again is idempotent
    CHECK(a.mergeAndRemap(b) == remap);
    CHECK(a.size() == 3);
    // merging into itself maps identity
    CHECK(a.mergeAndRemap(a) == std::vector<int>{0, 1, 2});
}

TEST_CASE("IdMap: addAllAllowingDups does not dedupe") {
    Fixture f;
    IdMap a, b;
    a.addIfAbsent(&f.b0, &f.k0);
    b.addIfAbsent(&f.b0, &f.k0);
    b.addIfAbsent(&f.b0, &f.k1);
    a.addAllAllowingDups(b);
    CHECK(a.size() == 3);
    CHECK(a.block(0) == &f.k0);
    CHECK(a.block(1) == &f.k0);
    CHECK(a.block(2) == &f.k1);
    // Java quirk: lookup points to the last duplicate
    CHECK(a.addIfAbsent(&f.b0, &f.k0) == 1);

    // copy into an empty map keeps ids aligned (the "just read, no merge" use)
    IdMap c;
    c.addAllAllowingDups(a);
    CHECK(c.sameEntries(a));
    IdMap d;
    d.addAllAllowingDups(d);
    CHECK(d.empty());
}

TEST_CASE("FullDataSource: createEmpty and indexing") {
    const FullDataSource s = FullDataSource::createEmpty(123);
    CHECK(s.pos == 123);
    CHECK(s.mapping.empty());
    CHECK(s.isEmpty);
    CHECK_FALSE(s.applyToParent.has_value());
    CHECK_FALSE(s.applyToChildren.has_value());
    CHECK_FALSE(s.regenerateLeaf.has_value());
    CHECK(s.createdUnixMs == 0);
    CHECK(s.lastModifiedUnixMs == 0);
    for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
        CHECK(s.columns[static_cast<std::size_t>(i)].empty());
        CHECK(s.columnGenerationStep[static_cast<std::size_t>(i)] == 0);
        CHECK(s.columnWorldCompressionMode[static_cast<std::size_t>(i)] == 0);
    }
    CHECK(FullDataSource::relativePosToIndex(0, 0) == 0);
    CHECK(FullDataSource::relativePosToIndex(1, 0) == 64);
    CHECK(FullDataSource::relativePosToIndex(3, 5) == 3 * 64 + 5);
    CHECK(FullDataSource::relativePosToIndex(63, 63) == 4095);
    CHECK(FullDataSource::tryRelativePosToIndex(-1, 0) == -1);
    CHECK(FullDataSource::tryRelativePosToIndex(0, 64) == -1);
    CHECK(FullDataSource::tryRelativePosToIndex(63, 63) == 4095);
    CHECK(s.tryColumn(64, 0) == nullptr);
    CHECK(s.tryColumn(2, 2) == &s.column(2, 2));
}

TEST_CASE("FullDataSource: setSingleColumn") {
    auto s = FullDataSource::createEmpty(1);
    s.setSingleColumn({fdp::encode(1, 2, 3, 4, 5)}, 10, 20, GenerationStep::Light, WorldCompressionMode::VisuallyEqual);
    const int i = FullDataSource::relativePosToIndex(10, 20);
    CHECK(s.columns[static_cast<std::size_t>(i)].size() == 1);
    CHECK(s.genStepAt(10, 20) == 9);
    CHECK(s.compModeAt(10, 20) == 1);
    CHECK(s.isEmpty); // like Java, setSingleColumn leaves the flag alone
    s.setSingleColumn({}, 10, 20, GenerationStep::DownSampled, WorldCompressionMode::MergeSameBlocks);
    CHECK(s.genStepAt(10, 20) == -1);
    s.setSingleColumn({1}, 64, 0, 3, 3); // out of range ignored
}

TEST_CASE("GenerationStep / CompressionMode enums") {
    CHECK(static_cast<int>(GenerationStep::DownSampled) == -1);
    CHECK(static_cast<int>(GenerationStep::Light) == 9);
    CHECK(static_cast<int>(GenerationStep::Terrain) == 7);
    CHECK(generationStepFromValue(7) == GenerationStep::LiquidCarvers);
    CHECK(generationStepFromValue(-1) == GenerationStep::DownSampled);
    CHECK_FALSE(generationStepFromValue(10).has_value());
    CHECK(compressionModeFromValue(1) == WorldCompressionMode::VisuallyEqual);
    CHECK_FALSE(compressionModeFromValue(2).has_value());
}

TEST_CASE("FullDataSource: column ordering helpers") {
    using Col = std::vector<std::uint64_t>;
    Col single{fdp::encode(0, 1, 5, 0, 0)};
    CHECK_FALSE(FullDataSource::isColumnInWrongOrder(single));
    Col empty;
    CHECK_FALSE(FullDataSource::isColumnInWrongOrder(empty));

    Col good{fdp::encode(0, 1, 9, 0, 0), fdp::encode(1, 1, 5, 0, 0), fdp::encode(2, 1, 1, 0, 0)};
    CHECK_FALSE(FullDataSource::isColumnInWrongOrder(good));
    Col copy = good;
    FullDataSource::ensureDataColumnOrder(copy);
    CHECK(copy == good);

    Col bad(good.rbegin(), good.rend());
    CHECK(FullDataSource::isColumnInWrongOrder(bad));
    FullDataSource::ensureDataColumnOrder(bad);
    CHECK(bad == good);

    // even length, reversed fully
    Col bad4{fdp::encode(0, 1, 1, 0, 0), fdp::encode(1, 1, 2, 0, 0), fdp::encode(2, 1, 3, 0, 0), fdp::encode(3, 1, 4, 0, 0)};
    FullDataSource::ensureDataColumnOrder(bad4);
    CHECK(fdp::id(bad4[0]) == 3);
    CHECK(fdp::id(bad4[3]) == 0);

    // equal first/last bottomY is not "wrong"
    Col eq{fdp::encode(0, 1, 4, 0, 0), fdp::encode(1, 1, 4, 0, 0)};
    CHECK_FALSE(FullDataSource::isColumnInWrongOrder(eq));
}

TEST_CASE("FullDataSource: clearAllNonAdjData (ported DataSourceRepoTests adjacency expectations)") {
    // Seeded like DataSourceRepoTests: pos irrelevant, java.util.Random(3), 1..31 points per column.
    for (AdjDirection dir : {AdjDirection::North, AdjDirection::South, AdjDirection::East, AdjDirection::West}) {
        auto src = FullDataSource::createEmpty(0x1234);
        JavaRandom r2(3);
        for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
            const int count = std::abs(r2.nextInt() % 31) + 1;
            auto& col = src.columns[static_cast<std::size_t>(i)];
            for (int c = 0; c < count; ++c)
                col.push_back(fdp::encode(c, 1, c, static_cast<std::uint8_t>(c % 15), static_cast<std::uint8_t>((c + 2) % 15)));
        }
        const auto original = src.columns;
        src.clearAllNonAdjData(dir);
        const AdjRange r = adjRange(dir);
        int adjCount = 0;
        for (int x = 0; x < 64; ++x) {
            for (int z = 0; z < 64; ++z) {
                const auto idx = static_cast<std::size_t>(FullDataSource::relativePosToIndex(x, z));
                const bool adj = x >= r.minX && x < r.maxX && z >= r.minZ && z < r.maxZ;
                if (adj) {
                    ++adjCount;
                    CHECK(src.columns[idx] == original[idx]);
                } else {
                    REQUIRE(src.columns[idx].size() == 1);
                    CHECK(src.columns[idx][0] == fdp::EMPTY_DATA_POINT);
                }
            }
        }
        CHECK(adjCount == 64);
    }
}

TEST_CASE("adjRange matches FullDataMinMaxPosUtil") {
    CHECK(adjRange(AdjDirection::North).maxZ == 1);
    CHECK(adjRange(AdjDirection::North).minZ == 0);
    CHECK(adjRange(AdjDirection::South).minZ == 63);
    CHECK(adjRange(AdjDirection::East).minX == 63);
    CHECK(adjRange(AdjDirection::West).maxX == 1);
    CHECK(adjRange(AdjDirection::West).maxZ == 64);
}

TEST_CASE("FullDataSource: contentHash stability and sensitivity") {
    auto make = [] {
        auto s = FullDataSource::createEmpty(42);
        for (int x = 0; x < 64; x += 7)
            for (int z = 0; z < 64; z += 5)
                s.setSingleColumn({fdp::encode(x, 3, z, 1, 2), fdp::encode(z, 2, 0, 3, 4)}, x, z, 9, 1);
        return s;
    };
    const auto a = make();
    const auto b = make();
    CHECK(a.contentHash() == b.contentHash());
    CHECK(a.contentEquals(b));
    // determinism across independent constructions
    CHECK(FullDataSource::createEmpty(0).contentHash() == FullDataSource::createEmpty(0).contentHash());
    CHECK(FullDataSource::createEmpty(1).contentHash() != FullDataSource::createEmpty(2).contentHash());

    auto c = make();
    c.columns[5].push_back(1);
    CHECK(c.contentHash() != a.contentHash());
    CHECK_FALSE(c.contentEquals(a));
    auto d = make();
    d.columnGenerationStep[100] = 3;
    CHECK(d.contentHash() != a.contentHash());
    auto e = make();
    e.columnWorldCompressionMode[100] = 1;
    CHECK(e.contentHash() != a.contentHash());
    // column boundaries matter: moving a point between adjacent columns changes the hash
    auto f = FullDataSource::createEmpty(1);
    auto g = FullDataSource::createEmpty(1);
    f.columns[0] = {7};
    g.columns[1] = {7};
    CHECK(f.contentHash() != g.contentHash());
    // mapping and flags are not part of the hash (as in Java)
    BiomeInfo bi{"x"};
    BlockInfo bk{"y"};
    auto h = make();
    h.mapping.addIfAbsent(&bi, &bk);
    h.applyToParent = true;
    CHECK(h.contentHash() == a.contentHash());
    CHECK(h.contentEquals(a));
}
