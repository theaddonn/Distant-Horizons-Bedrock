#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/storage/Codec.h"
#include "dh/util/Varint.h"

using namespace dh;

namespace {

constexpr int kRegistryBlocks = 64;
constexpr int kRegistryBiomes = 6;

struct Env {
    MapBlockRegistry              reg{false};
    std::vector<const BlockInfo*> blocks;
    std::vector<const BiomeInfo*> biomes;
    Env() {
        for (int i = 0; i < kRegistryBlocks; ++i) {
            BlockInfo b;
            b.serial = "minecraft:test_block_" + std::to_string(i);
            blocks.push_back(reg.addBlock(std::move(b)));
        }
        for (int i = 0; i < kRegistryBiomes; ++i) {
            BiomeInfo b;
            b.serial = "minecraft:test_biome_" + std::to_string(i);
            biomes.push_back(reg.addBiome(std::move(b)));
        }
    }
};

Env& env() {
    static Env e;
    return e;
}

/// Adds `n` distinct (biome, block) ids to the mapping.
void fillMapping(FullDataSource& s, int n) {
    for (int i = 0; i < n; ++i)
        s.mapping.addIfAbsent(
            env().biomes[static_cast<std::size_t>(i % kRegistryBiomes)],
            env().blocks[static_cast<std::size_t>(i % kRegistryBlocks)]
        );
}

/// Random valid top-down column (contiguous runs with occasional gaps and light).
std::vector<std::uint64_t> randomColumn(std::mt19937_64& rng, int mapN) {
    std::vector<std::uint64_t> col;
    const int                  kind = static_cast<int>(rng() % 10);
    if (kind == 0) return col; // empty list
    if (kind == 1) {
        col.push_back(fdp::EMPTY_DATA_POINT);
        return col;
    }
    int top = 200 + static_cast<int>(rng() % 3800);
    int n   = 1 + static_cast<int>(rng() % 8);
    for (int i = 0; i < n; ++i) {
        const int h = 1 + static_cast<int>(rng() % 60);
        if (top - h < 0) break;
        const int  bottom = top - h;
        const int  id     = static_cast<int>(rng() % static_cast<std::uint64_t>(mapN));
        const bool lit    = (rng() % 3) == 0;
        const auto bl     = static_cast<std::uint8_t>(lit ? rng() % 16 : 0);
        const auto sl     = static_cast<std::uint8_t>(lit ? rng() % 16 : 0);
        col.push_back(fdp::encode(id, h, bottom, bl, sl));
        top = bottom;
        if (rng() % 5 == 0) top -= static_cast<int>(rng() % 20); // gap
        if (top < 1) break;
    }
    return col;
}

FullDataSource randomSource(std::mt19937_64& rng, double fill) {
    FullDataSource s;
    s.pos          = rng();
    const int mapN = 1 + static_cast<int>(rng() % 60);
    fillMapping(s, mapN);
    const auto thr = static_cast<std::uint64_t>(fill * 1000.0);
    for (auto& c : s.columns) {
        if (rng() % 1000 < thr) c = randomColumn(rng, mapN);
    }
    int step = static_cast<int>(rng() % 10);
    int mode = static_cast<int>(rng() % 2);
    for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
        if (rng() % 200 == 0) step = static_cast<int>(rng() % 11) - 1;
        if (rng() % 500 == 0) mode = static_cast<int>(rng() % 2);
        s.columnGenerationStep[static_cast<std::size_t>(i)]       = static_cast<std::int8_t>(step);
        s.columnWorldCompressionMode[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(mode);
    }
    auto tri = [&]() -> std::optional<bool> {
        switch (rng() % 3) {
        case 0:
            return std::nullopt;
        case 1:
            return false;
        default:
            return true;
        }
    };
    s.applyToParent      = tri();
    s.applyToChildren    = tri();
    s.regenerateLeaf     = tri();
    s.createdUnixMs      = static_cast<std::int64_t>(rng() >> 2);
    s.lastModifiedUnixMs = static_cast<std::int64_t>(rng() >> 2);
    s.isEmpty            = false;
    return s;
}

void checkEqual(const FullDataSource& a, const FullDataSource& b) {
    CHECK(a.pos == b.pos);
    CHECK(a.contentEquals(b));
    CHECK(a.mapping.sameEntries(b.mapping));
    CHECK(a.applyToParent.value_or(false) == b.applyToParent.value_or(false));
    CHECK(a.applyToChildren.value_or(false) == b.applyToChildren.value_or(false));
    CHECK(a.regenerateLeaf.value_or(false) == b.regenerateLeaf.value_or(false));
    CHECK(a.createdUnixMs == b.createdUnixMs);
    CHECK(a.lastModifiedUnixMs == b.lastModifiedUnixMs);
}

constexpr Direction kHoriz[4] = {Direction::North, Direction::South, Direction::West, Direction::East};

void checkWellFormed(const FullDataSource& s) {
    for (const auto& c : s.columns) {
        for (std::uint64_t d : c) {
            if (d != fdp::EMPTY_DATA_POINT) REQUIRE(static_cast<std::size_t>(fdp::id(d)) < s.mapping.size());
        }
    }
    for (std::size_t i = 0; i < s.mapping.size(); ++i) {
        REQUIRE(s.mapping.block(static_cast<int>(i)) != nullptr);
        REQUIRE(s.mapping.biome(static_cast<int>(i)) != nullptr);
    }
}

/// Terrain-like fixture: smooth heightfield, grass/dirt/stone layers, water, sky light on top.
FullDataSource terrainSource() {
    FullDataSource s;
    s.pos = 0x1234;
    fillMapping(s, 12);
    for (int x = 0; x < 64; ++x) {
        for (int z = 0; z < 64; ++z) {
            const double fx = x / 64.0 * 6.2831853, fz = z / 64.0 * 6.2831853;
            const int    surface =
                100 + static_cast<int>(20.0 * std::sin(fx) * std::cos(fz) + 6.0 * std::sin(3 * fx + fz));
            std::vector<std::uint64_t> col;
            int                        top   = surface;
            const bool                 water = surface < 98;
            if (water) col.push_back(fdp::encode(5, 98 - surface, surface, 0, 15)); // water above the ground
            col.push_back(fdp::encode(1, 1, top - 1, 0, 15));                       // grass
            col.push_back(fdp::encode(2, 3, top - 4, 0, 0));                        // dirt
            col.push_back(fdp::encode(3, top - 4 - 5, 5, 0, 0));                    // stone
            col.push_back(fdp::encode(4, 5, 0, 0, 0));                              // bedrock-ish
            s.columns[static_cast<std::size_t>(x * 64 + z)]              = std::move(col);
            s.columnGenerationStep[static_cast<std::size_t>(x * 64 + z)] = 9;
        }
    }
    s.isEmpty = false;
    return s;
}

} // namespace

TEST_CASE("codec: empty and hand-built round trips") {
    {
        FullDataSource s = FullDataSource::createEmpty(42);
        auto           r = encode(s);
        REQUIRE(r.has_value());
        auto d = decode(*r, env().reg);
        REQUIRE(d.has_value());
        checkEqual(s, *d);
        CHECK(d->isEmpty);
        // an all-Light / mode-0 section: RLE blobs are tiny
        CHECK(r->genSteps.size() <= 8);
        CHECK(r->compressionModes.size() <= 8);
    }
    {
        FullDataSource s;
        s.pos = 7;
        fillMapping(s, 3);
        s.column(5, 9)       = {fdp::encode(2, 10, 50, 3, 12), fdp::encode(1, 5, 20, 0, 0)}; // gap below the first
        s.column(0, 0)       = {fdp::encode(0, 4095, 0, 15, 15)};
        s.column(63, 63)     = {fdp::EMPTY_DATA_POINT};
        s.column(63, 0)      = {fdp::encode(1, 1, 4094, 0, 1)};
        s.applyToParent      = true;
        s.createdUnixMs      = 1234567890123;
        s.lastModifiedUnixMs = 1234567999999;
        auto r               = encode(s);
        REQUIRE(r.has_value());
        CHECK(r->meta.hasFlag(StoreFlag::ApplyToParent));
        CHECK_FALSE(r->meta.hasFlag(StoreFlag::ApplyToChildren));
        CHECK(r->meta.dataFormatVersion == kCodecFormatVersion);
        auto d = decode(*r, env().reg);
        REQUIRE(d.has_value());
        checkEqual(s, *d);
        CHECK_FALSE(d->isEmpty);
    }
}

TEST_CASE("codec: rejects unrepresentable sources and zstd for now") {
    FullDataSource s;
    fillMapping(s, 2);
    s.column(3, 3) = {fdp::encode(5, 1, 1, 0, 0)}; // id outside the mapping
    auto r         = encode(s);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);

    FullDataSource ok = FullDataSource::createEmpty(1);
    CodecOptions   z;
    z.compression = Compression::Zstd;
    auto rz       = encode(ok, z);
    REQUIRE_FALSE(rz.has_value());
    CHECK(rz.error().code == ErrorCode::Unsupported);
}

TEST_CASE("codec: unknown serials fall back to air / default biome") {
    FullDataSource s;
    s.pos = 1;
    fillMapping(s, 4);
    s.column(10, 10) = {fdp::encode(3, 5, 5, 0, 0)};
    auto r           = encode(s);
    REQUIRE(r.has_value());
    MapBlockRegistry other(false); // knows only air/water/plains
    auto             d = decode(*r, other);
    REQUIRE(d.has_value());
    REQUIRE(d->mapping.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(d->mapping.block(i) == other.air());
        CHECK(d->mapping.biome(i) == other.defaultBiome());
    }
    CHECK(d->contentEquals(s));
}

TEST_CASE("codec: seeded fuzz round trip (1500 sources) + adjacent-only") {
    std::mt19937_64 rng(0xD1C0DECull);
    for (int i = 0; i < 1500; ++i) {
        const double   fill = (i % 100 == 0) ? 1.0 : (i % 3 == 0 ? 0.02 : 0.04);
        FullDataSource s    = randomSource(rng, fill);
        auto           r    = encode(s);
        REQUIRE(r.has_value());
        auto d = decode(*r, env().reg);
        REQUIRE(d.has_value());
        checkEqual(s, *d);
        if (i % 10 == 0) {
            for (Direction dir : kHoriz) {
                auto a = decodeAdjacentOnly(*r, dir, env().reg);
                REQUIRE(a.has_value());
                FullDataSource expect = *d;
                expect.clearAllNonAdjData(static_cast<AdjDirection>(compassIndex(dir)));
                CHECK(a->contentEquals(expect));
                CHECK(a->mapping.sameEntries(d->mapping));
            }
        }
    }
}

TEST_CASE("codec: adjacent-only needs neither data nor the other strips") {
    std::mt19937_64 rng(5);
    FullDataSource  s = randomSource(rng, 1.0);
    auto            r = encode(s);
    REQUIRE(r.has_value());
    auto full = decode(*r, env().reg);
    REQUIRE(full.has_value());
    for (Direction dir : kHoriz) {
        StoreRecord part = *r;
        part.data.clear();
        for (int k = 0; k < 4; ++k)
            if (k != compassIndex(dir)) part.adjacent[static_cast<std::size_t>(k)].clear();
        auto a = decodeAdjacentOnly(part, dir, env().reg);
        REQUIRE(a.has_value());
        FullDataSource expect = *full;
        expect.clearAllNonAdjData(static_cast<AdjDirection>(compassIndex(dir)));
        CHECK(a->contentEquals(expect));
    }
    CHECK_FALSE(decodeAdjacentOnly(*r, Direction::Up, env().reg).has_value());
}

TEST_CASE("codec: wire round trip") {
    std::mt19937_64 rng(99);
    for (int i = 0; i < 50; ++i) {
        FullDataSource s = randomSource(rng, 0.5);
        auto           r = encode(s);
        REQUIRE(r.has_value());
        std::vector<std::byte> buf;
        ByteWriter             w(buf);
        writeRecord(w, *r);
        ByteReader rd(buf);
        auto       back = readRecord(rd);
        REQUIRE(back.has_value());
        CHECK(rd.atEnd());
        CHECK(back->pos == r->pos);
        CHECK(back->data == r->data);
        CHECK(back->adjacent == r->adjacent);
        CHECK(back->genSteps == r->genSteps);
        CHECK(back->compressionModes == r->compressionModes);
        CHECK(back->mapping == r->mapping);
        CHECK(back->meta.flags == r->meta.flags);
        CHECK(back->meta.dataChecksum == r->meta.dataChecksum);
        CHECK(back->meta.createdMillis == r->meta.createdMillis);
        auto d = decode(*back, env().reg);
        REQUIRE(d.has_value());
        checkEqual(s, *d);
    }
}

namespace {

std::vector<std::byte>* blobAt(StoreRecord& r, int i) {
    switch (i) {
    case 0:
        return &r.data;
    case 1:
    case 2:
    case 3:
    case 4:
        return &r.adjacent[static_cast<std::size_t>(i - 1)];
    case 5:
        return &r.genSteps;
    case 6:
        return &r.compressionModes;
    default:
        return &r.mapping;
    }
}

/// Decodes (full and one adj direction); must not crash; a success must be well formed.
bool exerciseDecode(StoreRecord& rec, std::mt19937_64& rng) {
    if (rng() % 4 != 0) {
        if (auto c = computeDataChecksum(rec); c.has_value()) rec.meta.dataChecksum = *c; // reach the parser
    }
    auto d = decode(rec, env().reg);
    if (d.has_value()) checkWellFormed(*d);
    auto a = decodeAdjacentOnly(rec, kHoriz[rng() % 4], env().reg);
    if (a.has_value()) checkWellFormed(*a);
    return d.has_value();
}

} // namespace

TEST_CASE("codec: corruption - truncation at every byte offset") {
    std::mt19937_64 rng(3);
    for (int rec = 0; rec < 3; ++rec) {
        FullDataSource s = randomSource(rng, rec == 0 ? 0.003 : 0.05);
        auto           r = encode(s);
        REQUIRE(r.has_value());
        for (int b = 0; b < 8; ++b) {
            const std::size_t full   = blobAt(*r, b)->size();
            const std::size_t stride = rec == 0 ? (b == 0 ? 3 : 1) : std::max<std::size_t>(1, full / 80);
            for (std::size_t len = 0; len < full; len += stride) {
                StoreRecord m = *r;
                blobAt(m, b)->resize(len);
                exerciseDecode(m, rng);
            }
        }
        // wire: every strict prefix must be rejected
        std::vector<std::byte> buf;
        ByteWriter             w(buf);
        writeRecord(w, *r);
        for (std::size_t len = 0; len < buf.size(); ++len) {
            ByteReader rd(std::span<const std::byte>(buf.data(), len));
            CHECK_FALSE(readRecord(rd).has_value());
        }
    }
}

TEST_CASE("codec: corruption - random byte flips (2500 trials)") {
    std::mt19937_64          rng(0xBADF00Dull);
    std::vector<StoreRecord> recs;
    for (int i = 0; i < 4; ++i) {
        FullDataSource s = randomSource(rng, 0.05 + 0.1 * i);
        auto           r = encode(s);
        REQUIRE(r.has_value());
        recs.push_back(std::move(*r));
    }
    int rejected = 0, accepted = 0;
    for (int t = 0; t < 2500; ++t) {
        StoreRecord m     = recs[rng() % recs.size()];
        const int   flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) {
            auto* blob = blobAt(m, static_cast<int>(rng() % 8));
            if (blob->empty()) continue;
            const std::size_t at = rng() % blob->size();
            switch (rng() % 3) {
            case 0:
                (*blob)[at] = static_cast<std::byte>(rng() & 0xFF);
                break;
            case 1:
                (*blob)[at] ^= static_cast<std::byte>(1u << (rng() % 8));
                break;
            default:
                (*blob)[at] = static_cast<std::byte>(0xFF);
                break;
            }
        }
        if (rng() % 8 == 0) m.meta.dataChecksum = static_cast<std::uint32_t>(rng());
        (exerciseDecode(m, rng) ? accepted : rejected)++;
        if (t % 4 != 0) continue;

        // wire path
        std::vector<std::byte> buf;
        ByteWriter             w(buf);
        writeRecord(w, m);
        if (!buf.empty()) buf[rng() % buf.size()] ^= static_cast<std::byte>(1u << (rng() % 8));
        ByteReader rd(buf);
        if (auto back = readRecord(rd); back.has_value()) exerciseDecode(*back, rng);
    }
    MESSAGE("flip fuzz: rejected=" << rejected << " accepted(well-formed)=" << accepted);
}

TEST_CASE("codec: size report") {
    FullDataSource s = terrainSource();
    auto           r = encode(s);
    REQUIRE(r.has_value());
    auto d = decode(*r, env().reg);
    REQUIRE(d.has_value());
    CHECK(d->contentEquals(s));
    std::size_t adj = 0;
    for (const auto& a : r->adjacent) adj += a.size();
    std::size_t points = 0;
    for (const auto& c : s.columns) points += c.size();
    char buf[512];
    std::snprintf(
        buf,
        sizeof(buf),
        "terrain fixture (%zu points): data=%zu adj(4)=%zu genSteps=%zu modes=%zu mapping=%zu total=%zu bytes (%.2f "
        "B/point)",
        points,
        r->data.size(),
        adj,
        r->genSteps.size(),
        r->compressionModes.size(),
        r->mapping.size(),
        r->data.size() + adj + r->genSteps.size() + r->compressionModes.size() + r->mapping.size(),
        static_cast<double>(r->data.size() + adj) / static_cast<double>(points)
    );
    MESSAGE(buf);
}

TEST_CASE("codec: mapping with millions of empty strings is rejected") {
    FullDataSource s = FullDataSource::createEmpty(1);
    auto           r = encode(s);
    REQUIRE(r.has_value());
    std::vector<std::byte> payload;
    (void)varint::writeVarint(payload, 1u << 24);                                    // stringCount
    payload.insert(payload.end(), static_cast<std::size_t>(1u << 24), std::byte{0}); // empty strings
    (void)varint::writeVarint(payload, 0);                                           // pairCount
    std::vector<std::byte> blob{static_cast<std::byte>(kCodecFormatVersion), static_cast<std::byte>(0)};
    blob.insert(blob.end(), payload.begin(), payload.end());
    r->mapping = std::move(blob);
    auto d     = decode(*r, env().reg);
    CHECK_FALSE(d.has_value());
    CHECK_FALSE(decodeAdjacentOnly(*r, Direction::North, env().reg).has_value());
}
