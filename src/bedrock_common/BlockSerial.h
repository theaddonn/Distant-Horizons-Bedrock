#pragma once

// Bedrock Block / Biome -> canonical DH serial -> interned BlockInfo / BiomeInfo.
// Grammar: see dh/platform/BlockTable.h ("minecraft:snow_layer[height=3]").
//
// Only the state keys BlockTable::stateKeys(name) lists are emitted. Block::mSerializationId
// ("name" + "states" compound) is read directly; no exported function is called.

#include "dh/platform/BlockTable.h"

#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

class Block;
class Biome;

namespace dh::bedrock {

/// "minecraft:<name>" full name of the block type (adds the namespace if the game omits it).
std::string blockNameOf(const Block& block);

/// Canonical serial of `block`, emitting only `stateKeys` (must be sorted).
std::string serialOf(const Block& block, std::span<const std::string_view> stateKeys);

/// "minecraft:<name>" of the biome.
std::string biomeSerialOf(const Biome& biome);

/// Thread-safe cache Block*/Biome* -> interned info. Block and Biome objects are owned by the
/// game's registries and never move, so the raw pointer is a valid key for the process lifetime
/// (they are re-created on world switches only together with their registry; call clear() then).
class BlockSerializer {
public:
    explicit BlockSerializer(BlockTable& table) : table_(table) {}

    /// Never null: unknown blocks resolve to air (and are logged once per name).
    const BlockInfo* resolve(const Block& block);
    /// Never null: unknown biomes resolve to the default biome (logged once per name).
    const BiomeInfo* resolve(const Biome& biome);

    BlockTable& table() { return table_; }

    void clear();

    std::size_t cachedBlocks() const;
    std::size_t unknownBlockNames() const;

private:
    BlockTable&                                        table_;
    mutable std::shared_mutex                          mutex_;
    std::unordered_map<const Block*, const BlockInfo*> blocks_;
    std::unordered_map<const Biome*, const BiomeInfo*> biomes_;
    std::unordered_set<std::string>                    unknownBlocks_;
    std::unordered_set<std::string>                    unknownBiomes_;
};

} // namespace dh::bedrock
