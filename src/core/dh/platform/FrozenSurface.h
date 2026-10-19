#pragma once

// Glue-side chunk fix-up for snow-covered frozen water (not part of Java DH).
//
// Java's "water under ice takes the ice" rule (FullDataToRenderDataTransformer :443) keys on the block of the
// datapoint directly above the water. In Java snow can never lie on ice (SnowLayerBlock.canSurvive rejects the
// SNOW_LAYER_CANNOT_SURVIVE_ON tag: ice, packed ice, barrier), so the rule always sees the ice. Bedrock covers
// frozen oceans/rivers with snow layers ON the ice; the snow carry-over then replaces the ice block, the rule misses,
// and the water below stays a separate sky-light-0 datapoint (the "black navy lake").
//
// Run 13: the first fix (Run 12, `stripSnowOverFrozenWater`) dropped the snow instead. That fixed the dark water but
// turned every snowy frozen ocean into bare translucent ice (`#92B8FE`, alpha 190) over the dark sea floor: the
// lavender / blue-grey surface of the 2026-10-19 report, where vanilla shows white snow. Now the snow is KEPT and the
// water column directly below the ice is replaced by that ice block, which is exactly what Java's rule would have
// produced: snow (white, carried down with alpha 255) over one merged ice datapoint. The core stays byte-identical
// to Java (golden f2r); only the chunk adapters call this.

#include <cstdint>

#include "dh/platform/BlockInfo.h"

namespace dh {

inline bool isSnowLayerBlock(const BlockInfo* b) {
    return b != nullptr && b->serial.rfind("minecraft:snow_layer", 0) == 0;
}

/// `get(x, y, z)` returns the block, `set(x, y, z, block)` replaces it; y in [minY, maxY). For every snow layer whose
/// block below is ice and whose block below that is water, the contiguous water run under that ice is replaced by
/// the same ice block (the snow stays). Returns the number of blocks replaced.
template <class Get, class Set>
int freezeWaterUnderSnowCoveredIce(std::int32_t minY, std::int32_t maxY, Get&& get, Set&& set) {
    int n = 0;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (std::int32_t y = maxY - 1; y >= minY + 2; --y) {
                if (!isSnowLayerBlock(get(x, y, z))) continue;
                const BlockInfo* ice = get(x, y - 1, z);
                if (ice == nullptr || !ice->isIce) continue;
                std::int32_t w = y - 2;
                for (; w >= minY; --w) {
                    const BlockInfo* b = get(x, w, z);
                    if (b == nullptr || b->material != BlockMaterial::Water) break;
                    set(x, w, z, ice);
                    ++n;
                }
                y = w + 1; // continue below the replaced run (the loop's --y steps onto the first non-water block)
            }
        }
    }
    return n;
}

} // namespace dh
