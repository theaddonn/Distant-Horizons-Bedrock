#include "dh/platform/IBlockRegistry.h"

namespace dh {

MapBlockRegistry::MapBlockRegistry(bool autoCreateUnknown) : autoCreateUnknown_(autoCreateUnknown) {
    BlockInfo air;
    air.serial   = "minecraft:air";
    air.material = BlockMaterial::Air;
    air.opacity  = 0;
    air.isAir    = true;
    air.isSolid  = false;
    air_         = addBlock(std::move(air));

    BlockInfo water;
    water.serial   = "minecraft:water";
    water.material = BlockMaterial::Water;
    water.opacity  = 1;
    water.isSolid  = false;
    water.isLiquid = true;
    water_         = addBlock(std::move(water));

    defaultBiome_ = addBiome(BiomeInfo{"minecraft:plains"});
}

const BlockInfo* MapBlockRegistry::resolveBlock(std::string_view serial) {
    {
        std::lock_guard lock(mutex_);
        auto            it = blocks_.find(serial);
        if (it != blocks_.end()) return it->second.get();
        if (!autoCreateUnknown_) return nullptr;
    }
    BlockInfo info;
    info.serial = std::string(serial);
    return addBlock(std::move(info));
}

std::vector<const BlockInfo*> MapBlockRegistry::resolveAllStates(std::string_view baseName) {
    std::vector<const BlockInfo*> out;
    std::lock_guard               lock(mutex_);
    for (auto it = blocks_.lower_bound(baseName); it != blocks_.end(); ++it) {
        const std::string_view serial = it->first;
        if (serial.substr(0, baseName.size()) != baseName) break;
        if (serial.size() == baseName.size() || serial[baseName.size()] == '[') out.push_back(it->second.get());
    }
    return out;
}

const BiomeInfo* MapBlockRegistry::resolveBiome(std::string_view serial) {
    {
        std::lock_guard lock(mutex_);
        auto            it = biomes_.find(serial);
        if (it != biomes_.end()) return it->second.get();
        if (!autoCreateUnknown_) return nullptr;
    }
    return addBiome(BiomeInfo{std::string(serial)});
}

const BlockInfo* MapBlockRegistry::addBlock(BlockInfo info) {
    std::lock_guard lock(mutex_);
    auto            it = blocks_.find(info.serial);
    if (it != blocks_.end()) return it->second.get();
    std::string key = info.serial;
    auto        ptr = std::make_unique<BlockInfo>(std::move(info));
    return blocks_.emplace(std::move(key), std::move(ptr)).first->second.get();
}

const BiomeInfo* MapBlockRegistry::addBiome(BiomeInfo info) {
    std::lock_guard lock(mutex_);
    auto            it = biomes_.find(info.serial);
    if (it != biomes_.end()) return it->second.get();
    std::string key = info.serial;
    auto        ptr = std::make_unique<BiomeInfo>(std::move(info));
    return biomes_.emplace(std::move(key), std::move(ptr)).first->second.get();
}

std::size_t MapBlockRegistry::blockCount() const {
    std::lock_guard lock(mutex_);
    return blocks_.size();
}

std::size_t MapBlockRegistry::biomeCount() const {
    std::lock_guard lock(mutex_);
    return biomes_.size();
}

} // namespace dh
