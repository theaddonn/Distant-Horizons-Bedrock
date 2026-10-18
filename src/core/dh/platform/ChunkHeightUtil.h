#pragma once

// Java ChunkWrapper non-empty bounds and createDhHeightMaps, shared by every
// IChunk implementation (TestChunk, BedrockChunk) so the documented quirks live
// in exactly one place. Header-only templates over small callables; no virtual
// dispatch in the hot loops.

#include "dh/platform/BlockInfo.h"

#include <cstdint>

namespace dh::chunkheight {

struct NonEmptyBounds {
    std::int32_t minNonEmpty;
    std::int32_t maxNonEmpty;
};

/// `sectionEmpty(int section)` -> true if all 16 layers of section `s`
/// (y in [minY + s*16, minY + s*16 + 16)) are air. `maxY - minY` must be a positive multiple of 16.
///
/// Java getMinNonEmptyHeight: bottom-up, min Y of the first non-empty section, else minY.
/// Java getMaxNonEmptyHeight: top-down, the value is set to (section min + 16) BEFORE the
/// emptiness check and the loop never breaks early in the all-empty case, so an all-empty
/// chunk yields minY + 16 (not maxY as the Java doc comment claims). Keep.
template <class SectionEmpty>
NonEmptyBounds computeNonEmptyBounds(std::int32_t minY, std::int32_t maxY, SectionEmpty&& sectionEmpty) {
    const int      sectionCount = (maxY - minY) / 16;
    NonEmptyBounds out{minY, maxY};
    for (int s = 0; s < sectionCount; ++s) {
        if (!sectionEmpty(s)) {
            out.minNonEmpty = minY + s * 16;
            break;
        }
    }
    for (int s = sectionCount - 1; s >= 0; --s) {
        out.maxNonEmpty = minY + s * 16 + 16;
        if (!sectionEmpty(s)) break;
    }
    return out;
}

/// Verbatim port of ChunkWrapper.createDhHeightMaps. `blockAt(x, y, z)` returns a non-null
/// BlockInfo (air outside the build range). The block AT minNonEmpty is never examined (Java).
template <class BlockAt>
void computeHeightMaps(
    std::int32_t minNonEmpty,
    std::int32_t maxNonEmpty,
    BlockAt&&    blockAt,
    std::int32_t (&solid)[16][16],
    std::int32_t (&lightBlocking)[16][16]
) {
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            std::int32_t solidHeight         = minNonEmpty;
            std::int32_t lightBlockingHeight = minNonEmpty;

            std::int32_t     y     = maxNonEmpty;
            const BlockInfo* block = blockAt(x, y, z);
            while (y > minNonEmpty && (solidHeight == minNonEmpty || lightBlockingHeight == minNonEmpty)) {
                if (solidHeight == minNonEmpty && block->isSolid) solidHeight = y;
                if (lightBlockingHeight == minNonEmpty && block->opacity != 0) lightBlockingHeight = y;
                y--;
                block = blockAt(x, y, z);
            }
            solid[x][z]         = solidHeight;
            lightBlocking[x][z] = lightBlockingHeight;
        }
    }
}

} // namespace dh::chunkheight
