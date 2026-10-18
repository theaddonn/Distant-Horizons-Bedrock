#pragma once

// Port of util/WorldGenUtil.isPosInWorldGenRange, shared by the server request gate and the client
// queue's distance gate. Java read the generationMaxChunkRadius config inside the function and
// ignored its own parameter for the "no limit" test; here the radius parameter is the single source.

#include "dh/pos/BlockPos.h"
#include "dh/pos/ChunkPos.h"
#include "dh/pos/SectionPos.h"

#include <cstdint>

namespace dh::mp {

/// true when no radius is set (<= 0), else the section's Chebyshev signed block distance from the
/// centre chunk's centreBlockPos (sic: (8 + x) << 4, see ChunkPos) is within radius * 16 blocks.
inline bool isPosInWorldGenRange(
    std::uint64_t requestedPos,
    std::int32_t  centerChunkX,
    std::int32_t  centerChunkZ,
    std::int32_t  maxChunkRadius
) {
    if (maxChunkRadius <= 0) return true;
    const BlockPos     center = ChunkPos(centerChunkX, centerChunkZ).centerBlockPos();
    const std::int32_t dist   = sec::chebyshevSignedDistance(requestedPos, center.x, center.z);
    const std::int32_t maxR   = wmul(maxChunkRadius, lod::CHUNK_WIDTH);
    return dist <= maxR;
}

} // namespace dh::mp
