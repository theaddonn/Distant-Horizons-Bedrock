#include "dh/platform/BlockTable.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace dh {

namespace {

using Json = nlohmann::json;

struct VariantSpec {
    std::string_view baseName;
    std::string_view key;
    int              first, last; ///< inclusive integer range
};

constexpr VariantSpec kVariants[] = {
    {"minecraft:snow_layer", "height", 0, 7},
};
constexpr std::string_view kSnowKeys[] = {"height"};

std::string str(const Json& j, const char* key, const char* fallback = "") {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string(fallback);
}
bool boolean(const Json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}
int integer(const Json& j, const char* key, int fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? static_cast<int>(it->get<double>()) : fallback;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/// "#RRGGBBAA" -> 0xAARRGGBB, "#RRGGBB" -> 0xFFRRGGBB. False if malformed.
bool parseColor(const std::string& s, std::uint32_t& out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        int d = hexDigit(s[i]);
        if (d < 0) return false;
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    if (s.size() == 7) {
        out = 0xFF000000u | v;
        return true;
    }
    out = (v << 24) | (v >> 8); // RRGGBBAA -> AARRGGBB
    return true;
}

BlockMaterial parseMaterial(const std::string& s) {
    static const std::pair<std::string_view, BlockMaterial> kNames[] = {
        {"Unknown",     BlockMaterial::Unknown    },
        {"Leaves",      BlockMaterial::Leaves     },
        {"Stone",       BlockMaterial::Stone      },
        {"Wood",        BlockMaterial::Wood       },
        {"Metal",       BlockMaterial::Metal      },
        {"Dirt",        BlockMaterial::Dirt       },
        {"Lava",        BlockMaterial::Lava       },
        {"DeepSlate",   BlockMaterial::DeepSlate  },
        {"Snow",        BlockMaterial::Snow       },
        {"Sand",        BlockMaterial::Sand       },
        {"Terracotta",  BlockMaterial::Terracotta },
        {"NetherStone", BlockMaterial::NetherStone},
        {"Water",       BlockMaterial::Water      },
        {"Grass",       BlockMaterial::Grass      },
        {"Air",         BlockMaterial::Air        },
        {"Illuminated", BlockMaterial::Illuminated},
    };
    for (const auto& [name, mat] : kNames)
        if (name == s) return mat;
    return BlockMaterial::Unknown;
}

TintKind parseTint(const std::string& s) {
    if (s == "grass") return TintKind::Grass;
    if (s == "foliage") return TintKind::Foliage;
    if (s == "water") return TintKind::Water;
    if (s == "birch") return TintKind::Birch;
    if (s == "evergreen") return TintKind::Evergreen;
    if (s == "dry_foliage") return TintKind::DryFoliage;
    return TintKind::None;
}

std::uint32_t colorOr(const Json& j, const char* key, std::uint32_t fallback) {
    std::uint32_t c  = 0;
    auto          it = j.find(key);
    if (it != j.end() && it->is_string() && parseColor(it->get<std::string>(), c)) return c;
    return fallback;
}

} // namespace

std::span<const std::string_view> BlockTable::stateKeys(std::string_view baseName) {
    if (baseName == "minecraft:snow_layer") return kSnowKeys;
    return {};
}

const BlockInfo* BlockTable::addBlock(BlockInfo info, const BlockColorInfo& color) {
    auto  ptr = std::make_unique<BlockInfo>(std::move(info));
    auto* raw = ptr.get();
    blocks_.push_back(std::move(ptr));
    blockBySerial_.emplace(raw->serial, raw);
    blockColors_.emplace(raw, color);
    if (auto br = raw->serial.find('['); br != std::string::npos)
        variantsByBase_[raw->serial.substr(0, br)].push_back(raw);
    return raw;
}

Result<BlockTable> BlockTable::load(std::string_view blockTableJson, std::string_view biomeTableJson) {
    Json blocksDoc = Json::parse(blockTableJson.begin(), blockTableJson.end(), nullptr, false);
    if (blocksDoc.is_discarded() || !blocksDoc.is_object())
        return makeError(ErrorCode::Corrupt, "block_table.json: not a JSON object");
    Json biomesDoc = Json::parse(biomeTableJson.begin(), biomeTableJson.end(), nullptr, false);
    if (biomesDoc.is_discarded() || !biomesDoc.is_object())
        return makeError(ErrorCode::Corrupt, "biome_table.json: not a JSON object");

    auto blocksIt = blocksDoc.find("blocks");
    if (blocksIt == blocksDoc.end() || !blocksIt->is_object())
        return makeError(ErrorCode::Corrupt, "block_table.json: missing \"blocks\" object");
    auto biomesIt = biomesDoc.find("biomes");
    if (biomesIt == biomesDoc.end() || !biomesIt->is_object())
        return makeError(ErrorCode::Corrupt, "biome_table.json: missing \"biomes\" object");

    BlockTable t;
    t.blocks_.reserve(blocksIt->size() + 16);

    for (auto it = blocksIt->begin(); it != blocksIt->end(); ++it) {
        const Json& j = it.value();
        if (!j.is_object())
            return makeError(ErrorCode::Corrupt, "block_table.json: entry is not an object: " + it.key());

        BlockInfo info;
        info.serial        = it.key();
        info.material      = parseMaterial(str(j, "material"));
        info.opacity       = static_cast<std::uint8_t>(std::clamp(integer(j, "opacity", 16), 0, 16));
        info.lightEmission = static_cast<std::uint8_t>(std::clamp(integer(j, "lightEmission", 0), 0, 15));
        info.isAir         = boolean(j, "isAir", false);
        info.isSolid       = boolean(j, "isSolid", true);
        info.isLiquid      = boolean(j, "isLiquid", false);
        info.isIce         = boolean(j, "isIce", false);

        BlockColorInfo c;
        c.argb = colorOr(j, "color", 0xFFFFFFFFu);
        c.up = c.side = c.down = c.argb;
        if (auto fc = j.find("faceColors"); fc != j.end() && fc->is_object()) {
            c.up   = colorOr(*fc, "up", c.argb);
            c.side = colorOr(*fc, "side", c.argb);
            c.down = colorOr(*fc, "down", c.argb);
        }
        c.tint = parseTint(str(j, "tint", "none"));
        if (auto tf = j.find("tintFaces"); tf != j.end() && tf->is_array()) {
            for (const Json& f : *tf) {
                if (!f.is_string()) continue;
                const std::string face = f.get<std::string>();
                if (face == "up") c.tintFaces |= kTintFaceUp;
                else if (face == "side") c.tintFaces |= kTintFaceSide;
                else if (face == "down") c.tintFaces |= kTintFaceDown;
            }
        }
        if (c.tint != TintKind::None && c.tintFaces == 0) c.tintFaces = kTintFaceUp | kTintFaceSide | kTintFaceDown;

        t.addBlock(std::move(info), c);
    }

    // state variants (copy of the base entry under the stateful serial)
    for (const VariantSpec& v : kVariants) {
        auto base = t.blockBySerial_.find(v.baseName);
        if (base == t.blockBySerial_.end()) continue;
        const BlockInfo*     baseInfo  = base->second;
        const BlockColorInfo baseColor = t.blockColors_.at(baseInfo);
        for (int value = v.first; value <= v.last; ++value) {
            BlockInfo copy = *baseInfo;
            copy.serial    = std::string(v.baseName) + "[" + std::string(v.key) + "=" + std::to_string(value) + "]";
            t.addBlock(std::move(copy), baseColor);
        }
    }

    if (auto a = t.blockBySerial_.find("minecraft:air"); a != t.blockBySerial_.end()) t.air_ = a->second;
    if (auto w = t.blockBySerial_.find("minecraft:water"); w != t.blockBySerial_.end()) t.water_ = w->second;
    if (!t.air_ || !t.water_)
        return makeError(ErrorCode::Corrupt, "block_table.json: minecraft:air / minecraft:water missing");

    // biomes
    BiomeTint defaultTint;
    if (auto d = biomesDoc.find("default"); d != biomesDoc.end() && d->is_object())
        defaultTint.water = colorOr(*d, "waterColor", defaultTint.water);
    for (auto it = biomesIt->begin(); it != biomesIt->end(); ++it) {
        const Json& j = it.value();
        if (!j.is_object())
            return makeError(ErrorCode::Corrupt, "biome_table.json: entry is not an object: " + it.key());
        auto      ptr        = std::make_unique<BiomeInfo>(BiomeInfo{it.key()});
        BiomeTint bt         = defaultTint;
        bt.grass             = colorOr(j, "grassTint", bt.grass);
        bt.foliage           = colorOr(j, "foliageTint", bt.foliage);
        bt.water             = colorOr(j, "waterColor", bt.water);
        bt.birch             = colorOr(j, "birchTint", bt.birch);
        bt.evergreen         = colorOr(j, "evergreenTint", bt.evergreen);
        bt.dryFoliage        = colorOr(j, "dryFoliageTint", bt.dryFoliage);
        const BiomeInfo* raw = ptr.get();
        t.biomes_.push_back(std::move(ptr));
        t.biomeBySerial_.emplace(raw->serial, raw);
        t.biomeTints_.emplace(raw, bt);
    }
    auto plains = t.biomeBySerial_.find("minecraft:plains");
    if (plains == t.biomeBySerial_.end())
        return makeError(ErrorCode::Corrupt, "biome_table.json: minecraft:plains missing");
    t.defaultBiome_ = plains->second;

    return t;
}

const BlockInfo* BlockTable::resolveBlock(std::string_view serial) {
    if (auto it = blockBySerial_.find(serial); it != blockBySerial_.end()) return it->second;
    const std::size_t bracket = serial.find('[');
    if (bracket == std::string_view::npos) return nullptr;
    if (auto it = blockBySerial_.find(serial.substr(0, bracket)); it != blockBySerial_.end()) return it->second;
    return nullptr;
}

std::vector<const BlockInfo*> BlockTable::resolveAllStates(std::string_view baseName) {
    std::vector<const BlockInfo*> out;
    if (auto it = blockBySerial_.find(baseName); it != blockBySerial_.end()) out.push_back(it->second);
    if (auto it = variantsByBase_.find(baseName); it != variantsByBase_.end())
        out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
}

const BiomeInfo* BlockTable::resolveBiome(std::string_view serial) {
    auto it = biomeBySerial_.find(serial);
    return it == biomeBySerial_.end() ? nullptr : it->second;
}

const BlockColorInfo* BlockTable::colorInfo(const BlockInfo* info) const {
    auto it = blockColors_.find(info);
    return it == blockColors_.end() ? nullptr : &it->second;
}

const BiomeTint* BlockTable::biomeTint(const BiomeInfo* info) const {
    if (info == nullptr) info = defaultBiome_;
    auto it = biomeTints_.find(info);
    return it == biomeTints_.end() ? nullptr : &it->second;
}

} // namespace dh
