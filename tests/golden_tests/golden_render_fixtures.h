// Shared fixtures for the render replays (f2r, mesh): a block registry keyed by resource name (the
// dumper's CSVs and snow-layer detection need them), the dumper's colour-hash level and render-source decoding.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/render/ColumnRenderSource.h"
#include "golden_loader.h"

namespace dh::test::gr {

using nlohmann::json;

/// block_table.gv.json as an IBlockRegistry whose serials are the dumper's resource names (serial 0..24).
/// air() = serial 0, water() = serial 4. block(i) / biome(i) make it usable with gf::fromGoldenSource.
class RenderRegistry final : public IBlockRegistry {
public:
    static RenderRegistry* get() {
        static std::unique_ptr<RenderRegistry> inst = load();
        return inst.get();
    }
    const BlockInfo* block(int serial) const { return blocks_.at(static_cast<std::size_t>(serial)).get(); }
    const BiomeInfo* biome(int serial) const { return biomes_.at(static_cast<std::size_t>(serial)).get(); }
    int              blockSerial(const BlockInfo* b) const { return blockIdx_.at(b); }
    int              biomeSerial(const BiomeInfo* b) const { return biomeIdx_.at(b); }

    const BlockInfo* resolveBlock(std::string_view serial) override {
        auto it = byName_.find(std::string(serial));
        return it == byName_.end() ? nullptr : it->second;
    }
    const BiomeInfo* resolveBiome(std::string_view) override { return nullptr; }
    const BlockInfo* air() override { return block(0); }
    const BlockInfo* water() override { return block(4); }
    const BiomeInfo* defaultBiome() override { return biome(0); }

private:
    static std::unique_ptr<RenderRegistry> load() {
        auto t = loadGolden("block_table");
        if (!t || t->cases.empty()) return nullptr;
        std::unique_ptr<RenderRegistry> r(new RenderRegistry());
        for (const json& b : t->cases[0].out.at("blocks")) {
            auto      i      = std::make_unique<BlockInfo>();
            const int serial = b.at("serial").get<int>();
            i->serial        = b.at("resourceName").get<std::string>();
            i->material      = static_cast<BlockMaterial>(b.at("materialId").get<int>());
            i->opacity       = static_cast<std::uint8_t>(b.at("opacity").get<int>());
            i->lightEmission = static_cast<std::uint8_t>(b.at("lightEmission").get<int>());
            i->isAir         = b.at("isAir").get<bool>();
            i->isSolid       = b.at("isSolid").get<bool>();
            i->isLiquid      = b.at("isLiquid").get<bool>();
            i->isIce         = b.at("isIce").get<bool>();
            if (static_cast<std::size_t>(serial) != r->blocks_.size()) return nullptr;
            r->byName_[i->serial] = i.get();
            r->blockIdx_[i.get()] = serial;
            r->blocks_.push_back(std::move(i));
        }
        const int nb = t->cases[0].out.at("biomeCount").get<int>();
        for (int s = 0; s < nb; ++s) {
            auto b                = std::make_unique<BiomeInfo>(BiomeInfo{"M" + std::to_string(s)});
            r->biomeIdx_[b.get()] = s;
            r->biomes_.push_back(std::move(b));
        }
        return r;
    }
    RenderRegistry() = default;

    std::vector<std::unique_ptr<BlockInfo>>           blocks_;
    std::vector<std::unique_ptr<BiomeInfo>>           biomes_;
    std::unordered_map<std::string, const BlockInfo*> byName_;
    std::unordered_map<const BlockInfo*, int>         blockIdx_;
    std::unordered_map<const BiomeInfo*, int>         biomeIdx_;
};

/// DumpClientLevel: colour = 0xFF000000 | ((b * 0x9E3779B1) ^ (m * 0x85EBCA6B)) & 0xFFFFFF (32-bit), dirt = (2, 0),
/// vanilla shades (Down .5, Up 1, N/S .8, W/E .6; face-index order).
class DumperLevel final : public IClientLevel {
public:
    DumperLevel(RenderRegistry& reg, int minY, int maxY, bool ceiling, bool end)
    : reg_(reg),
      minY_(minY),
      maxY_(maxY),
      ceiling_(ceiling),
      end_(end) {}

    std::array<float, 6> shades = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};

    static std::uint32_t hashColor(int b, int m) {
        const std::uint32_t h =
            (static_cast<std::uint32_t>(b) * 0x9E3779B1u) ^ (static_cast<std::uint32_t>(m) * 0x85EBCA6Bu);
        return 0xFF000000u | (h & 0xFFFFFFu);
    }

    std::int32_t     minHeight() const override { return minY_; }
    std::int32_t     maxHeight() const override { return maxY_; }
    bool             hasCeiling() const override { return ceiling_; }
    bool             hasSkyLight() const override { return true; }
    bool             isEnd() const override { return end_; }
    std::string_view dimensionName() const override { return "dump"; }
    std::uint32_t
    blockColor(std::int32_t, std::int32_t, std::int32_t, const BiomeInfo* biome, const BlockInfo* block) override {
        return hashColor(reg_.blockSerial(block), reg_.biomeSerial(biome));
    }
    std::uint32_t dirtBlockColor() override { return hashColor(2, 0); }
    float         shade(Direction face) const override { return shades[static_cast<std::size_t>(faceIndex(face))]; }

private:
    RenderRegistry& reg_;
    int             minY_, maxY_;
    bool            ceiling_, end_;
};

/// f2r `out` shape (+ pos) -> ColumnRenderSource. Slots beyond the dumped (trimmed) points are empty.
inline ColumnRenderSource renderSourceFromJson(const json& j, std::uint64_t pos) {
    const int          slices = j.at("maxVerticalSlices").get<int>();
    ColumnRenderSource s      = ColumnRenderSource::createEmpty(pos, slices, j.at("yOffset").get<int>());
    s.isEmpty                 = false;
    std::vector<std::vector<std::uint64_t>> table;
    for (const json& t : j.at("dataTable")) table.push_back(hexU64Array(t));
    int n = 0;
    for (const json& r : j.at("dataRuns")) {
        const auto& t = table.at(r.at(0).get<std::size_t>());
        for (int k = 0; k < r.at(1).get<int>(); ++k, ++n) {
            const int x = n / 64, z = n % 64;
            for (std::size_t i = 0; i < t.size(); ++i)
                s.data[static_cast<std::size_t>(ColumnRenderSource::dataIndex(x, z, slices, static_cast<int>(i)))] =
                    t[i];
        }
    }
    const std::vector<std::uint64_t> vt = hexU64Array(j.at("voidTable"));
    int                              m  = 0;
    for (const json& r : j.at("voidRuns"))
        for (int k = 0; k < r.at(1).get<int>(); ++k, ++m)
            s.voidData[static_cast<std::size_t>(m)] = vt.at(r.at(0).get<std::size_t>());
    return s;
}

} // namespace dh::test::gr
