#pragma once

// Bedrock world-generation facts the distant generator reproduces (not part of Java DH). From static RE of
// Minecraft Bedrock 1.26.51 (docs/PORTING.md "Run 13 (LOD content)"):
//
// * Decoration chunk seed (OverworldGenerator::decorationPostProcessChunk, all arithmetic 32-bit):
//     Random r(levelSeed); xs = r.nextInt() / 2 * 2 + 1; zs = r.nextInt() / 2 * 2 + 1;
//     seed = levelSeed ^ (chunkX * xs + chunkZ * zs)
//   (the compiled form is `(a + (a >>> 31)) | 1`, identical to a/2*2+1 with C++ truncating division).
// * Decoration order (scheduler, ChunkStateNeighborDependency::DecorationParity, tables selected by
//   (x & 1) + 2 * (z & 1)): class 0 = (even, even) needs no decorated neighbour, class 1 = (odd, even) needs its two
//   x neighbours decorated, class 2 = (even, odd) needs the rows z-1 and z+1 decorated, class 3 = (odd, odd) needs all
//   eight. Decorating by ascending class therefore satisfies every dependency.

#include <cstdint>

namespace dh {

/// `a / 2 * 2 + 1` on the 32-bit `nextInt()` draw (odd multiplier).
constexpr std::uint32_t decorationSeedMultiplier(std::int32_t a) {
    return static_cast<std::uint32_t>(a / 2 * 2 + 1);
}

/// levelSeed ^ (chunkX * xs + chunkZ * zs), wrapping 32-bit.
constexpr std::uint32_t decorationChunkSeed(
    std::uint32_t levelSeed,
    std::uint32_t xs,
    std::uint32_t zs,
    std::int32_t  chunkX,
    std::int32_t  chunkZ
) {
    return levelSeed ^ (static_cast<std::uint32_t>(chunkX) * xs + static_cast<std::uint32_t>(chunkZ) * zs);
}

/// 0..3, decoration runs in ascending class order.
constexpr int decorationParityClass(std::int32_t chunkX, std::int32_t chunkZ) {
    return (chunkX & 1) + 2 * (chunkZ & 1);
}

} // namespace dh
