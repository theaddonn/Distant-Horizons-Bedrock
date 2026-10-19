#pragma once

// Glue-side chunk fix-up for frozen lakes (not part of Java DH).
//
// Java's "water under ice takes the ice" rule (FullDataToRenderDataTransformer :443) keys on the block of the
// datapoint directly above the water. A snow layer lying on the ice replaces that block through the snow
// carry-over, so the rule misses, the water stays visible and its sky light is 0 (ice is opaque to the DH sky
// bake): the lake renders as a near-black navy slab. The core must stay byte-identical to Java (golden f2r), so
// the chunk adapters drop snow layers that lie on ice-over-water before the chunk is turned into full data.

#include <cstdint>
#include <string_view>

#include "dh/platform/BlockInfo.h"

namespace dh {

inline bool isSnowLayerBlock(const BlockInfo* b) {
    return b != nullptr && b->serial.rfind("minecraft:snow_layer", 0) == 0;
}

/// `get(x, y, z)` returns the block, `set(x, y, z, block)` replaces it; y in [minY, maxY). Replaces every snow
/// layer whose block below is ice and whose block below that is water with `air`. Returns the number replaced.
template <class Get, class Set>
int stripSnowOverFrozenWater(std::int32_t minY, std::int32_t maxY, const BlockInfo* air, Get&& get, Set&& set) {
    int n = 0;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (std::int32_t y = minY + 2; y < maxY; ++y) {
                if (!isSnowLayerBlock(get(x, y, z))) continue;
                const BlockInfo* ice = get(x, y - 1, z);
                if (ice == nullptr || !ice->isIce) continue;
                const BlockInfo* below = get(x, y - 2, z);
                if (below == nullptr || below->material != BlockMaterial::Water) continue;
                set(x, y, z, air);
                ++n;
            }
        }
    }
    return n;
}

} // namespace dh
