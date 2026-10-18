#include "bedrock_common/BlockSerial.h"

#include "bedrock_common/FileLogger.h"

#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include <mutex>

namespace dh::bedrock {

namespace {

std::string withNamespace(std::string name) {
    if (name.find(':') == std::string::npos) name.insert(0, "minecraft:");
    return name;
}

void appendValue(std::string& out, const CompoundTagVariant& v) {
    if (v.hold<IntTag>()) {
        out += std::to_string(v.get<IntTag>().data);
    } else if (v.hold<ByteTag>()) {
        out += v.get<ByteTag>().data != 0 ? "true" : "false";
    } else if (v.hold<StringTag>()) {
        out += static_cast<std::string const&>(v.get<StringTag>());
    } else {
        out += "?";
    }
}

constexpr std::size_t kMaxUnknownLogs = 64;

} // namespace

std::string blockNameOf(const Block& block) {
    return withNamespace(block.getBlockType().mNameInfo->mFullName->getString());
}

std::string serialOf(const Block& block, std::span<const std::string_view> stateKeys) {
    std::string out = blockNameOf(block);
    if (stateKeys.empty()) return out;

    CompoundTag const& tag = *block.mSerializationId;
    if (!tag.contains("states")) return out;
    CompoundTagVariant const& states = tag["states"];
    if (!states.is_object()) return out;

    bool first = true;
    for (std::string_view key : stateKeys) {
        auto const& items = states.items();
        auto        it    = items.find(key);
        if (it == items.end()) continue;
        out   += first ? '[' : ',';
        first  = false;
        out   += key;
        out   += '=';
        appendValue(out, it->second);
    }
    if (!first) out += ']';
    return out;
}

std::string biomeSerialOf(const Biome& biome) { return withNamespace(biome.mHash->getString()); }

const BlockInfo* BlockSerializer::resolve(const Block& block) {
    {
        std::shared_lock lock(mutex_);
        auto             it = blocks_.find(&block);
        if (it != blocks_.end()) return it->second;
    }
    // slow path, once per distinct Block*
    const std::string name   = blockNameOf(block);
    const std::string serial = serialOf(block, BlockTable::stateKeys(name));
    const BlockInfo*  info   = table_.resolveBlock(serial);

    std::unique_lock lock(mutex_);
    if (info == nullptr) {
        if (unknownBlocks_.size() < kMaxUnknownLogs && unknownBlocks_.insert(serial).second)
            DH_LOG_W("block not in block_table.json, treated as air: %s", serial.c_str());
        info = table_.air();
    }
    blocks_.emplace(&block, info);
    return info;
}

const BiomeInfo* BlockSerializer::resolve(const Biome& biome) {
    {
        std::shared_lock lock(mutex_);
        auto             it = biomes_.find(&biome);
        if (it != biomes_.end()) return it->second;
    }
    const std::string serial = biomeSerialOf(biome);
    const BiomeInfo*  info   = table_.resolveBiome(serial);

    std::unique_lock lock(mutex_);
    if (info == nullptr) {
        if (unknownBiomes_.size() < kMaxUnknownLogs && unknownBiomes_.insert(serial).second)
            DH_LOG_W("biome not in biome_table.json, using default: %s", serial.c_str());
        info = table_.defaultBiome();
    }
    biomes_.emplace(&biome, info);
    return info;
}

void BlockSerializer::clear() {
    std::unique_lock lock(mutex_);
    blocks_.clear();
    biomes_.clear();
}

std::size_t BlockSerializer::cachedBlocks() const {
    std::shared_lock lock(mutex_);
    return blocks_.size();
}

std::size_t BlockSerializer::unknownBlockNames() const {
    std::shared_lock lock(mutex_);
    return unknownBlocks_.size();
}

} // namespace dh::bedrock
