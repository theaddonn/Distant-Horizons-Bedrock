#pragma once

// Block / biome interning (Java: IWrapperFactory.deserializeBlockStateWrapper,
// deserializeBiomeWrapper, getAirBlockStateWrapper, getWaterBlockStateWrapper,
// getPlainsBiomeWrapper).
//
// Contract:
//   * Pointers are stable for the registry's lifetime and pointer equality is
//     state equality. The core stores pointers in IdMaps and compares them.
//   * resolveBlock/resolveBiome return nullptr for an unknown serial; the
//     *OrDefault helpers mirror Java's "fall back to AIR / plains" behaviour.
//   * air(), water(), defaultBiome() are never null.
//   * Implementations must be safe to call from multiple threads concurrently
//     (decode runs on worker pools).
//
// Implementations: glue BlockTable (data/block_table.json + Bedrock palette),
// MapBlockRegistry below (tests/tools).

#include "dh/platform/BlockInfo.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dh {

struct IBlockRegistry {
    virtual ~IBlockRegistry() = default;

    /// nullptr if the platform cannot resolve `serial`.
    virtual const BlockInfo* resolveBlock(std::string_view serial) = 0;
    /// nullptr if the platform cannot resolve `serial`.
    virtual const BiomeInfo* resolveBiome(std::string_view serial) = 0;

    /// Every known state of the block whose base name (text before '[', e.g. "minecraft:kelp")
    /// equals `baseName` (Java getAllBlockWrappers). Empty if unknown. The default only does an
    /// exact resolveBlock; implementations that can enumerate states should override.
    virtual std::vector<const BlockInfo*> resolveAllStates(std::string_view baseName) {
        const BlockInfo* b = resolveBlock(baseName);
        if (b == nullptr) return {};
        return {b};
    }

    virtual const BlockInfo* air()          = 0;
    virtual const BlockInfo* water()        = 0;
    virtual const BiomeInfo* defaultBiome() = 0;

    const BlockInfo* resolveBlockOrAir(std::string_view serial) {
        const BlockInfo* b = resolveBlock(serial);
        return b ? b : air();
    }
    const BiomeInfo* resolveBiomeOrDefault(std::string_view serial) {
        const BiomeInfo* b = resolveBiome(serial);
        return b ? b : defaultBiome();
    }
};

/// Simple std::map-backed registry for tests and host tools.
///
/// Constructed with "minecraft:air" (isAir, opacity 0, not solid, material Air),
/// "minecraft:water" (liquid, opacity 1, not solid, material Water) and biome
/// "minecraft:plains". Blocks/biomes are added with add*(); with
/// `autoCreateUnknown` an unknown serial is interned on first resolve as a
/// generic solid opaque Unknown block (handy for golden fixtures that only care
/// about identity).
class MapBlockRegistry final : public IBlockRegistry {
public:
    explicit MapBlockRegistry(bool autoCreateUnknown = false);

    const BlockInfo* resolveBlock(std::string_view serial) override;
    /// Scan of the sorted map: serials equal to baseName or baseName + "[...]".
    std::vector<const BlockInfo*> resolveAllStates(std::string_view baseName) override;
    const BiomeInfo* resolveBiome(std::string_view serial) override;
    const BlockInfo* air() override { return air_; }
    const BlockInfo* water() override { return water_; }
    const BiomeInfo* defaultBiome() override { return defaultBiome_; }

    /// Interns `info` under info.serial. If the serial already exists the
    /// existing entry is returned unchanged (pointers must stay stable).
    const BlockInfo* addBlock(BlockInfo info);
    const BiomeInfo* addBiome(BiomeInfo info);

    std::size_t blockCount() const;
    std::size_t biomeCount() const;

private:
    mutable std::mutex                                             mutex_;
    std::map<std::string, std::unique_ptr<BlockInfo>, std::less<>> blocks_;
    std::map<std::string, std::unique_ptr<BiomeInfo>, std::less<>> biomes_;
    const BlockInfo*                                               air_          = nullptr;
    const BlockInfo*                                               water_        = nullptr;
    const BiomeInfo*                                               defaultBiome_ = nullptr;
    bool                                                           autoCreateUnknown_;
};

} // namespace dh
