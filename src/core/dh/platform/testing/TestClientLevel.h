#pragma once

// IClientLevel double: colour = 0xFF000000 | (fnv(blockSerial) ^ fnv(biomeSerial)*31) & 0xFFFFFF unless
// overridden per block; Java's default shades. Deterministic.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "dh/platform/BlockInfo.h"
#include "dh/platform/ILevelInfo.h"

namespace dh {

class TestClientLevel : public IClientLevel {
public:
    std::int32_t minY = -64, maxY = 320;
    bool         ceiling = false, skyLight = true, end = false;
    std::string  name = "minecraft:overworld";
    /// block -> exact colour (alpha included)
    std::map<const BlockInfo*, std::uint32_t> colorOverride;

    std::int32_t     minHeight() const override { return minY; }
    std::int32_t     maxHeight() const override { return maxY; }
    bool             hasCeiling() const override { return ceiling; }
    bool             hasSkyLight() const override { return skyLight; }
    bool             isEnd() const override { return end; }
    std::string_view dimensionName() const override { return name; }

    std::uint32_t defaultColor(const BiomeInfo* biome, const BlockInfo* block) const {
        std::uint32_t h = hashSerial(block->serial);
        if (biome != nullptr) h ^= hashSerial(biome->serial) * 31u;
        return 0xFF000000u | (h & 0xFFFFFFu);
    }
    std::uint32_t
    blockColor(std::int32_t, std::int32_t, std::int32_t, const BiomeInfo* biome, const BlockInfo* block) override {
        auto it = colorOverride.find(block);
        return it != colorOverride.end() ? it->second : defaultColor(biome, block);
    }
    std::uint32_t dirtBlockColor() override { return 0xFF866043u; }
    float         shade(Direction face) const override {
        switch (face) {
        case Direction::Up:
            return 1.0f;
        case Direction::Down:
            return 0.5f;
        case Direction::North:
        case Direction::South:
            return 0.8f;
        default:
            return 0.6f;
        }
    }
};

} // namespace dh
