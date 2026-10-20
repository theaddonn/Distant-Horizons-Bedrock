#pragma once

// Bedrock world-generation facts the distant generator reproduces (not part of Java DH). From static RE of
// Minecraft Bedrock 1.26.51 (docs/PORTING.md "Run 13 (LOD content)"):
//
// * Decoration chunk seed (OverworldGenerator::decorationPostProcessChunk, all arithmetic 32-bit):
//     Random r(levelSeed); xs = r.nextInt() / 2 * 2 + 1; zs = r.nextInt() / 2 * 2 + 1;
//     seed = levelSeed ^ (chunkX * xs + chunkZ * zs)
//   (the compiled form is `(a + (a >>> 31)) | 1`, identical to a/2*2+1 with C++ truncating division).
// * Decoration order (scheduler 0x29dddd0, ChunkStateNeighborDependency::DecorationParity tables @0xe840350..68,
//   8 bytes = minimum neighbour state, 4 = StructurePostProcessed, 6 = DecorationPostProcessed). Run 15 re-traced the
//   table selection (cmov chain at 0x29debe3..0x29dec2e): (even, even) -> T0 (all eight neighbours only structure
//   post-processed), (odd, even) -> T1 (its two x neighbours decorated), (odd, odd) -> T2 (the six chunks of rows
//   z-1 / z+1 decorated), (even, odd) -> T3 (all eight decorated). Run 13 had the last two swapped. Decorating by
//   ascending class (e,e) < (o,e) < (o,o) < (e,o) satisfies every dependency, and every pair of adjacent chunks of
//   different classes is strictly ordered. Same-class chunks are never adjacent; their order (shared border chunks
//   of their 3x3 views) is not fixed by vanilla (ready queue sorted by ChunkBuildOrderPolicy priority, i.e. load
//   order around the player), so any fixed order is as good as vanilla's.
// * Every class also needs all eight neighbours structure post-processed (state 4) before it decorates, and the
//   structure pass of a chunk needs its 3x3 at Generated (state 2).

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

/// 0..3, decoration runs in ascending class order: (even, even) 0, (odd, even) 1, (odd, odd) 2, (even, odd) 3.
constexpr int decorationParityClass(std::int32_t chunkX, std::int32_t chunkZ) {
    const int ox = chunkX & 1, oz = chunkZ & 1;
    return oz == 0 ? ox : 3 - ox;
}

} // namespace dh
