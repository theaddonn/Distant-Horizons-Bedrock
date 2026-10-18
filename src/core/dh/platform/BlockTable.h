#pragma once

// IBlockRegistry backed by the generated data/block_table.json + data/biome_table.json
// (see tools/block-table/README.md). Immutable after load(): every query is lock-free and
// safe from any thread; returned pointers live as long as the BlockTable object (moving the
// BlockTable keeps them valid).
//
// Canonical block serial grammar (BlockInfo::serial, what the glue produces and resolveBlock accepts):
//
//     serial := name [ "[" state { "," state } "]" ]
//     name   := namespace ":" path            e.g. "minecraft:stone"
//     state  := key "=" value                 e.g. "height=3"
//
// State pairs are sorted by key (byte order) with no spaces. The table only distinguishes the
// state keys listed by stateKeys(name) (today: `height` on minecraft:snow_layer, values 0..7);
// the glue must emit ONLY those keys so unrelated states (facing, waterlogged, ...) do not
// explode the number of interned blocks. Examples: "minecraft:snow_layer[height=3]",
// "minecraft:grass_block".
//
// resolveBlock(): exact serial first; if absent and the serial carries a state list, the
// base name (everything before '[') is tried; otherwise nullptr. Variants share every property
// of their base entry (the table is per block id); they exist so the serial, and thus the
// chunk hash and the stored palette, keep the layer height (PORTING.md snow-layer note: the
// Java snow rule looks for `layers=N` on minecraft:snow, Bedrock has
// `minecraft:snow_layer[height=N]`; the transformer must match this serial).
//
// Biome serials are "namespace:name" ("minecraft:plains"); resolveBiome accepts exactly the
// ids of biome_table.json.

#include "dh/platform/IBlockRegistry.h"
#include "dh/util/Result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dh {

enum class TintKind : std::uint8_t { None = 0, Grass, Foliage, Water, Birch, Evergreen, DryFoliage };

/// Bits of BlockColorInfo::tintFaces.
inline constexpr std::uint8_t kTintFaceUp   = 1;
inline constexpr std::uint8_t kTintFaceSide = 2;
inline constexpr std::uint8_t kTintFaceDown = 4;

/// Colours are 0xAARRGGBB. `argb` is the whole-block colour (untinted when tint != None).
struct BlockColorInfo {
    std::uint32_t argb = 0;
    std::uint32_t up = 0, side = 0, down = 0;
    TintKind      tint      = TintKind::None;
    std::uint8_t  tintFaces = 0;
};

/// Biome tints as 0xFFRRGGBB.
struct BiomeTint {
    std::uint32_t grass = 0xFFFFFFFF, foliage = 0xFFFFFFFF, water = 0xFFFFFFFF;
    std::uint32_t birch = 0xFFFFFFFF, evergreen = 0xFFFFFFFF, dryFoliage = 0xFFFFFFFF;
};

class BlockTable final : public IBlockRegistry {
public:
    /// Parses both documents. Errors: Corrupt (bad JSON/shape, missing air/water/plains).
    static Result<BlockTable> load(std::string_view blockTableJson, std::string_view biomeTableJson);

    BlockTable(BlockTable&&) noexcept            = default;
    BlockTable& operator=(BlockTable&&) noexcept = default;
    BlockTable(const BlockTable&)                = delete;
    BlockTable& operator=(const BlockTable&)     = delete;

    const BlockInfo* resolveBlock(std::string_view serial) override;
    const BiomeInfo* resolveBiome(std::string_view serial) override;
    const BlockInfo* air() override { return air_; }
    const BlockInfo* water() override { return water_; }
    const BiomeInfo* defaultBiome() override { return defaultBiome_; }

    /// Base entry (if any) plus every state variant of `baseName` (Java getAllBlockWrappers). O(1) index.
    /// Matches IBlockRegistry::resolveAllStates once that virtual lands on main (override then implicit).
    std::vector<const BlockInfo*> resolveAllStates(std::string_view baseName);

    /// nullptr if `info` was not produced by this table.
    const BlockColorInfo* colorInfo(const BlockInfo* info) const;
    /// nullptr if `info` was not produced by this table. biomeTint(nullptr) is the default biome's.
    const BiomeTint* biomeTint(const BiomeInfo* info) const;

    /// State keys (sorted) the table distinguishes for `baseName` ("minecraft:snow_layer" -> {"height"}); empty
    /// otherwise.
    static std::span<const std::string_view> stateKeys(std::string_view baseName);

    std::size_t blockCount() const { return blocks_.size(); } ///< including synthesized state variants
    std::size_t biomeCount() const { return biomes_.size(); }

private:
    BlockTable() = default;

    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    const BlockInfo* addBlock(BlockInfo info, const BlockColorInfo& color);

    std::vector<std::unique_ptr<BlockInfo>>                                                     blocks_;
    std::vector<std::unique_ptr<BiomeInfo>>                                                     biomes_;
    std::unordered_map<std::string, const BlockInfo*, StringHash, std::equal_to<>>              blockBySerial_;
    std::unordered_map<std::string, const BiomeInfo*, StringHash, std::equal_to<>>              biomeBySerial_;
    std::unordered_map<std::string, std::vector<const BlockInfo*>, StringHash, std::equal_to<>> variantsByBase_;
    std::unordered_map<const BlockInfo*, BlockColorInfo>                                        blockColors_;
    std::unordered_map<const BiomeInfo*, BiomeTint>                                             biomeTints_;
    const BlockInfo*                                                                            air_          = nullptr;
    const BlockInfo*                                                                            water_        = nullptr;
    const BiomeInfo*                                                                            defaultBiome_ = nullptr;
};

} // namespace dh
