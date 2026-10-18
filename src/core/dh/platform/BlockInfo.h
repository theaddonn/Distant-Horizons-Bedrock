#pragma once

#include <cstdint>
#include <string>

namespace dh {

/// Material classes used by the render data point (4 bits). Mirrors Java
/// EDhApiBlockMaterial; the numeric values are part of the vertex format.
enum class BlockMaterial : std::uint8_t {
    Unknown     = 0,
    Leaves      = 1,
    Stone       = 2,
    Wood        = 3,
    Metal       = 4,
    Dirt        = 5,
    Lava        = 6,
    DeepSlate   = 7,
    Snow        = 8,
    Sand        = 9,
    Terracotta  = 10,
    NetherStone = 11,
    Water       = 12,
    Grass       = 13,
    Air         = 14,
    Illuminated = 15,
};

/// Everything the core needs to know about a block state. Interned by the
/// platform's IBlockRegistry: pointers are stable for the lifetime of the
/// registry and pointer equality means state equality.
///
/// Replaces Java IBlockStateWrapper. `serial` is the platform's canonical
/// string for the state (Bedrock: "minecraft:stone[stone_type=granite]") and
/// is the only field persisted; the rest is re-derived on load.
struct BlockInfo {
    std::string   serial;
    BlockMaterial material      = BlockMaterial::Unknown;
    std::uint8_t  opacity       = 16; ///< 0 = fully transparent, 16 = fully opaque (Java getOpacity)
    std::uint8_t  lightEmission = 0;  ///< 0..15
    bool          isAir         = false;
    bool          isSolid       = true;
    bool          isLiquid      = false;
    bool          isIce         = false;

    /// Java IBlockStateWrapper.isAir() || isLiquid() etc. are often queried together.
    bool isFullyOpaque() const { return opacity >= 16; }
};

/// Biome identity (Java IBiomeWrapper). Only the serial is needed by the core;
/// tinting happens in the platform's colour provider.
struct BiomeInfo {
    std::string serial;
};

} // namespace dh
