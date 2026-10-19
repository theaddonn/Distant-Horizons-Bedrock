#include "dh/platform/TableClientLevel.h"

#include "dh/util/Color.h"

namespace dh {

std::uint32_t TableClientLevel::tintColor(TintKind kind, const BiomeInfo* biome) const {
    const BiomeTint* t = table_.biomeTint(biome);
    if (t == nullptr) return 0xFFFFFFFFu;
    switch (kind) {
    case TintKind::Grass:
        return t->grass;
    case TintKind::Foliage:
        return t->foliage;
    case TintKind::Water:
        return t->water;
    case TintKind::Birch:
        return t->birch;
    case TintKind::Evergreen:
        return t->evergreen;
    case TintKind::DryFoliage:
        return t->dryFoliage;
    case TintKind::None:
        break;
    }
    return 0xFFFFFFFFu;
}

std::uint32_t
TableClientLevel::blockColor(std::int32_t, std::int32_t, std::int32_t, const BiomeInfo* biome, const BlockInfo* block) {
    const BlockColorInfo* c = table_.colorInfo(block);
    if (c == nullptr) return 0; // not a table block
    if (c->tint == TintKind::None) return c->argb;
    return color::multiplyARGBwithRGB(c->argb, tintColor(c->tint, biome));
}

std::uint32_t TableClientLevel::dirtBlockColor() {
    const BlockInfo* dirt = table_.resolveBlock("minecraft:dirt");
    if (dirt == nullptr) return 0xFFFFFFFFu;
    return blockColor(0, 0, 0, nullptr, dirt);
}

float TableClientLevel::shade(Direction face) const {
    switch (face) {
    case Direction::Down:
        return 0.5f;
    case Direction::Up:
        return 1.0f;
    case Direction::North:
    case Direction::South:
        return 0.8f;
    case Direction::West:
    case Direction::East:
        return 0.6f;
    }
    return 1.0f;
}

} // namespace dh
