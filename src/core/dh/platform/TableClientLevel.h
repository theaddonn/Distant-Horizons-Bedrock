#pragma once

// IClientLevel backed by BlockTable colours (Java ClientLevelWrapper.getBlockColor +
// ClientBlockStateColorCache.getColor semantics):
//   colour = table base colour; if the block has a tint kind, multiplyARGBwithRGB(base, biome tint)
//   (alpha of the base colour is kept; water therefore stays translucent).
// Unknown block (not from the table) -> 0 (invisible). Unknown/null biome -> the default biome's tints.
//
// TODO(lodBiomeBlending): Java's `lodBiomeBlending` radius averaging of the tint over neighbouring
// biomes is not implemented; the tint of the column's own biome is used.
//
// Thread-safety: everything is read-only after construction, so any thread may call.

#include "dh/platform/BlockTable.h"
#include "dh/platform/ILevelInfo.h"

#include <cstdint>
#include <string>

namespace dh {

/// Plain level facts, filled by the glue from the game's dimension.
struct LevelInfo {
    std::int32_t minHeight     = -64; ///< inclusive
    std::int32_t maxHeight     = 320; ///< exclusive
    bool         hasCeiling    = false;
    bool         hasSkyLight   = true;
    bool         isEnd         = false;
    std::string  dimensionName = "minecraft:overworld";
    std::int64_t hashedSeed    = 0;
};

class TableClientLevel : public IClientLevel {
public:
    /// `table` must outlive this object.
    TableClientLevel(BlockTable& table, LevelInfo info) : table_(table), info_(std::move(info)) {}

    std::int32_t     minHeight() const override { return info_.minHeight; }
    std::int32_t     maxHeight() const override { return info_.maxHeight; }
    bool             hasCeiling() const override { return info_.hasCeiling; }
    bool             hasSkyLight() const override { return info_.hasSkyLight; }
    bool             isEnd() const override { return info_.isEnd; }
    std::string_view dimensionName() const override { return info_.dimensionName; }
    std::int64_t     hashedSeed() const override { return info_.hashedSeed; }

    std::uint32_t blockColor(
        std::int32_t     blockX,
        std::int32_t     blockY,
        std::int32_t     blockZ,
        const BiomeInfo* biome,
        const BlockInfo* block
    ) override;

    /// Colour of minecraft:dirt (untinted), 0xFFFFFFFF if the table has none.
    std::uint32_t dirtBlockColor() override;

    /// Vanilla constants (Java ClientLevelWrapper.getShade, "ENABLED"/AUTO): down 0.5, up 1.0,
    /// north/south 0.8, west/east 0.6.
    float shade(Direction face) const override;

    /// Tint colour (0xFFRRGGBB) of `kind` in `biome`; 0xFFFFFFFF for TintKind::None.
    std::uint32_t tintColor(TintKind kind, const BiomeInfo* biome) const;

private:
    BlockTable& table_;
    LevelInfo   info_;
};

} // namespace dh
