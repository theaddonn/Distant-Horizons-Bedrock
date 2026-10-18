// Shared fixtures for the full-data golden replays (merge/cull, chunk_build, lighting).
// Builds C++ objects from the dumper's JSON (tools/golden-dumper/README.md) and compares C++ state
// against the dumped one. Compare helpers return "" on a match, otherwise a description of the FIRST
// difference, so that a 4096-column comparison costs a single doctest assertion.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IChunk.h"
#include "dh/platform/testing/TestChunk.h"
#include "golden_loader.h"

namespace dh::test::gf {

using nlohmann::json;

/// tests/golden/block_table.gv.json as a MapBlockRegistry. Blocks are interned as "B<serial>", biomes as "M<serial>"
/// (the dumper's getSerialString). The two air ids 0 (air) and 15 (cave_air) are two distinct registry entries that
/// both have isAir/opacity 0; the registry's own built-in "minecraft:air" is never used by the replays.
class GoldenRegistry {
public:
    /// nullptr when block_table.gv.json is missing.
    static GoldenRegistry* get() {
        static std::unique_ptr<GoldenRegistry> inst = load();
        return inst.get();
    }

    MapBlockRegistry& registry() { return reg_; }
    const BlockInfo*  block(int serial) const { return blocks_.at(static_cast<std::size_t>(serial)); }
    const BiomeInfo*  biome(int serial) const { return biomes_.at(static_cast<std::size_t>(serial)); }
    static int        serialOf(const BlockInfo* b) { return std::stoi(b->serial.substr(1)); }
    static int        serialOf(const BiomeInfo* b) { return std::stoi(b->serial.substr(1)); }

private:
    static std::unique_ptr<GoldenRegistry> load() {
        auto t = loadGolden("block_table");
        if (!t || t->cases.empty()) return nullptr;
        std::unique_ptr<GoldenRegistry> r(new GoldenRegistry());
        for (const json& b : t->cases[0].out.at("blocks")) {
            BlockInfo i;
            const int serial = b.at("serial").get<int>();
            i.serial         = "B" + std::to_string(serial);
            i.material       = static_cast<BlockMaterial>(b.at("materialId").get<int>());
            i.opacity        = static_cast<std::uint8_t>(b.at("opacity").get<int>());
            i.lightEmission  = static_cast<std::uint8_t>(b.at("lightEmission").get<int>());
            i.isAir          = b.at("isAir").get<bool>();
            i.isSolid        = b.at("isSolid").get<bool>();
            i.isLiquid       = b.at("isLiquid").get<bool>();
            i.isIce          = b.at("isIce").get<bool>();
            if (static_cast<std::size_t>(serial) != r->blocks_.size()) return nullptr;
            r->blocks_.push_back(r->reg_.addBlock(std::move(i)));
        }
        const int nb = t->cases[0].out.at("biomeCount").get<int>();
        for (int s = 0; s < nb; ++s) r->biomes_.push_back(r->reg_.addBiome(BiomeInfo{"M" + std::to_string(s)}));
        return r;
    }
    GoldenRegistry() = default;

    MapBlockRegistry              reg_;
    std::vector<const BlockInfo*> blocks_;
    std::vector<const BiomeInfo*> biomes_;
};

inline std::vector<std::uint64_t> longs(const json& a) {
    std::vector<std::uint64_t> v;
    v.reserve(a.size());
    for (const json& e : a) v.push_back(hexU64(e));
    return v;
}

/// Dumper "Ds" json -> FullDataSource (createWithData semantics: isEmpty = false). Unlisted columns are empty with
/// defGen/defComp (0 when absent); the mapping is built in id order with IdMap::addAllAllowingDups.
inline std::unique_ptr<FullDataSource> fromGoldenSource(const json& ds, GoldenRegistry& reg) {
    auto s     = std::make_unique<FullDataSource>();
    s->pos     = hexU64(ds.at("pos"));
    s->isEmpty = false;
    IdMap m(s->pos);
    for (const json& p : ds.at("palette")) {
        IdMap one(s->pos);
        one.addIfAbsent(reg.biome(p.at("biome").get<int>()), reg.block(p.at("block").get<int>()));
        m.addAllAllowingDups(one);
    }
    s->mapping        = std::move(m);
    const int defGen  = ds.value("defGen", 0);
    const int defComp = ds.value("defComp", 0);
    for (std::size_t i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
        s->columnGenerationStep[i]       = static_cast<std::int8_t>(defGen);
        s->columnWorldCompressionMode[i] = static_cast<std::int8_t>(defComp);
    }
    for (const json& c : ds.at("cols")) {
        const auto i                     = c.at(0).get<std::size_t>();
        s->columnGenerationStep[i]       = static_cast<std::int8_t>(c.at(1).get<int>());
        s->columnWorldCompressionMode[i] = static_cast<std::int8_t>(c.at(2).get<int>());
        s->columns[i]                    = longs(c.at(3));
    }
    return s;
}

/// propagate: Ds json with an optional `fill {gen, comp, pts}` = the column (genStep, compMode, datapoints) every
/// UNLISTED column starts with. Without `fill` this is fromGoldenSource.
inline std::unique_ptr<FullDataSource> fromGoldenSourceFilled(const json& ds, GoldenRegistry& reg) {
    auto s = fromGoldenSource(ds, reg);
    if (!ds.contains("fill")) return s;
    const json&                      fill = ds.at("fill");
    const std::vector<std::uint64_t> pts  = longs(fill.at("pts"));
    std::vector<bool>                listed(FullDataSource::COLUMN_COUNT, false);
    for (const json& c : ds.at("cols")) listed[c.at(0).get<std::size_t>()] = true;
    for (std::size_t i = 0; i < listed.size(); ++i) {
        if (listed[i]) continue;
        s->columnGenerationStep[i]       = static_cast<std::int8_t>(fill.at("gen").get<int>());
        s->columnWorldCompressionMode[i] = static_cast<std::int8_t>(fill.at("comp").get<int>());
        s->columns[i]                    = pts;
    }
    return s;
}

inline std::string hx(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llx", static_cast<unsigned long long>(v));
    return b;
}

inline std::string colStr(const std::vector<std::uint64_t>& c) {
    std::string s = "[";
    for (std::uint64_t v : c) s += hx(v) + ",";
    return s + "]";
}

/// Mapping as serial pairs in id order vs json palette [{block,biome}].
inline std::string diffPalette(const IdMap& m, const json& pal) {
    if (m.size() != pal.size())
        return "palette size C++ " + std::to_string(m.size()) + " vs Java " + std::to_string(pal.size());
    for (std::size_t i = 0; i < pal.size(); ++i) {
        const int jb = pal[i].at("block").get<int>(), jm = pal[i].at("biome").get<int>();
        const int cb = GoldenRegistry::serialOf(m.block(static_cast<int>(i)));
        const int cm = GoldenRegistry::serialOf(m.biome(static_cast<int>(i)));
        if (jb != cb || jm != cm)
            return "palette[" + std::to_string(i) + "] C++ (block " + std::to_string(cb) + ", biome "
                 + std::to_string(cm) + ") vs Java (" + std::to_string(jb) + ", " + std::to_string(jm) + ")";
    }
    return "";
}

/// Compares all 4096 columns (datapoints, genStep, compMode) against a dumped Ds (sparse cols + defGen/defComp).
inline std::string diffColumns(const FullDataSource& s, const json& ds) {
    const int                defGen  = ds.value("defGen", 0);
    const int                defComp = ds.value("defComp", 0);
    std::vector<const json*> listed(FullDataSource::COLUMN_COUNT, nullptr);
    for (const json& c : ds.at("cols")) listed[c.at(0).get<std::size_t>()] = &c;
    for (std::size_t i = 0; i < listed.size(); ++i) {
        int eg = defGen, ec = defComp;
        if (listed[i]) {
            eg = (*listed[i])[1].get<int>();
            ec = (*listed[i])[2].get<int>();
        }
        const bool gOk = s.columnGenerationStep[i] == eg, cOk = s.columnWorldCompressionMode[i] == ec;
        const bool pOk = listed[i] ? s.columns[i] == longs((*listed[i])[3]) : s.columns[i].empty();
        if (gOk && cOk && pOk) continue;
        const auto where =
            "column " + std::to_string(i) + " (x=" + std::to_string(i / 64) + ",z=" + std::to_string(i % 64) + "): ";
        if (!pOk)
            return where + "datapoints C++ " + colStr(s.columns[i]) + " vs Java "
                 + (listed[i] ? colStr(longs((*listed[i])[3])) : std::string("[]"));
        if (!gOk)
            return where + "genStep C++ " + std::to_string(s.columnGenerationStep[i]) + " vs Java "
                 + std::to_string(eg);
        return where + "compMode C++ " + std::to_string(s.columnWorldCompressionMode[i]) + " vs Java "
             + std::to_string(ec);
    }
    return "";
}

/// chunk_build: out.colTable/colRuns (run-length of table index over x*16+z; source column =
/// (offX+x)*64 + offZ+z).
inline std::string diffChunkColumns(const FullDataSource& s, const json& out, int offX, int offZ) {
    std::vector<std::vector<std::uint64_t>> table;
    for (const json& t : out.at("colTable")) table.push_back(longs(t));
    int n = 0;
    for (const json& r : out.at("colRuns")) {
        const auto& t = table.at(r.at(0).get<std::size_t>());
        for (int k = 0; k < r.at(1).get<int>(); ++k, ++n) {
            const int  x = n / 16, z = n % 16;
            const auto di = static_cast<std::size_t>((offX + x) * 64 + offZ + z);
            if (s.columns[di] != t)
                return "chunk column x=" + std::to_string(x) + ",z=" + std::to_string(z) + ": C++ "
                     + colStr(s.columns[di]) + " vs Java " + colStr(t);
        }
    }
    if (n != 256) return "colRuns cover " + std::to_string(n) + " columns";
    return "";
}

/// genStep/compMode are uniform over the chunk's 256 columns in the dump.
inline std::string diffChunkUniform(const FullDataSource& s, int offX, int offZ, int gen, int comp) {
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z) {
            const auto di = static_cast<std::size_t>((offX + x) * 64 + offZ + z);
            if (s.columnGenerationStep[di] != gen)
                return "genStep at x=" + std::to_string(x) + ",z=" + std::to_string(z) + " C++ "
                     + std::to_string(s.columnGenerationStep[di]) + " vs Java " + std::to_string(gen);
            if (s.columnWorldCompressionMode[di] != comp)
                return "compMode at x=" + std::to_string(x) + ",z=" + std::to_string(z) + " C++ "
                     + std::to_string(s.columnWorldCompressionMode[di]) + " vs Java " + std::to_string(comp);
        }
    return "";
}

/// Number of columns outside the chunk's 16x16 area holding datapoints (Java otherNonEmpty).
inline int countOtherNonEmpty(const FullDataSource& s, int offX, int offZ) {
    int n = 0;
    for (int i = 0; i < FullDataSource::COLUMN_COUNT; ++i) {
        const int  x = i / 64, z = i % 64;
        const bool inside = x >= offX && x < offX + 16 && z >= offZ && z < offZ + 16;
        if (!inside && !s.columns[static_cast<std::size_t>(i)].empty()) ++n;
    }
    return n;
}

/// ChunkFx json -> TestChunk. profiles[p] = run-length [[blockSerial,count],..] from minY upward; colRuns = run-length
/// of profile index over x*16+z; biomeRuns[qx*4+qz] = run-length [[biome,count],..] over 4-high cells from minY (cell
/// index clamped to the last cell). Block-light positions are derived by TestChunk from lightEmission.
inline std::unique_ptr<TestChunk> fromGoldenChunk(const json& c, GoldenRegistry& reg) {
    const int minY = c.at("minY").get<int>(), maxY = c.at("maxY").get<int>();
    auto      ch = std::make_unique<TestChunk>(
        c.at("chunkX").get<int>(),
        c.at("chunkZ").get<int>(),
        minY,
        maxY,
        reg.block(0),
        reg.biome(0)
    );
    int n = 0;
    for (const json& r : c.at("colRuns")) {
        const json& prof = c.at("profiles").at(r.at(0).get<std::size_t>());
        for (int k = 0; k < r.at(1).get<int>(); ++k, ++n) {
            const int x = n / 16, z = n % 16;
            int       y = minY;
            for (const json& run : prof) {
                const int cnt = run.at(1).get<int>();
                const int to  = std::min(y + cnt, maxY);
                if (run.at(0).get<int>() != 0 && y < to) ch->fillColumn(x, z, y, to, reg.block(run.at(0).get<int>()));
                y += cnt;
                if (y >= maxY) break;
            }
        }
    }
    const json& br = c.at("biomeRuns");
    for (int q = 0; q < 16; ++q) {
        std::vector<int> cells;
        for (const json& run : br.at(static_cast<std::size_t>(q)))
            for (int k = 0; k < run.at(1).get<int>(); ++k) cells.push_back(run.at(0).get<int>());
        if (cells.empty()) continue;
        const int qx = q / 4, qz = q % 4;
        for (int y = minY; y < maxY; ++y) {
            const int cell  = std::min<int>((y - minY) >> 2, static_cast<int>(cells.size()) - 1);
            const int biome = cells[static_cast<std::size_t>(cell)];
            if (biome == 0) continue; // TestChunk starts as biome 0
            for (int dx = 0; dx < 4; ++dx)
                for (int dz = 0; dz < 4; ++dz) ch->setBiome(qx * 4 + dx, y, qz * 4 + dz, reg.biome(biome));
        }
    }
    return ch;
}

/// Light arrays of the chunk (v = sky*16 + block, y = minY..maxY-1) vs out.colTable/colRuns.
inline std::string diffLight(const IChunk& ch, const json& out) {
    const int minY = ch.inclusiveMinBuildHeight(), maxY = ch.exclusiveMaxBuildHeight();
    int       n = 0;
    for (const json& r : out.at("colRuns")) {
        const json& runs = out.at("colTable").at(r.at(0).get<std::size_t>());
        for (int k = 0; k < r.at(1).get<int>(); ++k, ++n) {
            const int x = n / 16, z = n % 16;
            int       y = minY;
            for (const json& run : runs) {
                for (int j = 0; j < run.at(1).get<int>(); ++j, ++y) {
                    const int v = run.at(0).get<int>();
                    if (y >= maxY) return "light table longer than build height";
                    const int cv = ch.dhSkyLight(x, y, z) * 16 + ch.dhBlockLight(x, y, z);
                    if (cv != v)
                        return "light x=" + std::to_string(x) + ",y=" + std::to_string(y) + ",z=" + std::to_string(z)
                             + " C++ sky " + std::to_string(cv >> 4) + "/block " + std::to_string(cv & 15)
                             + " vs Java sky " + std::to_string(v >> 4) + "/block " + std::to_string(v & 15);
                }
            }
            if (y != maxY) return "light table shorter than build height";
        }
    }
    return n == 256 ? "" : "colRuns cover " + std::to_string(n) + " columns";
}

} // namespace dh::test::gf
