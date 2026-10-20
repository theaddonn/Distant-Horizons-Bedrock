#pragma once

// Which chunks the Bedrock distant generator's Features mode builds, structure-processes, decorates and delivers for
// one square batch, and in which order (Bedrock port, not part of Java DH; docs/PORTING.md "Run 15").
//
// Vanilla's per-chunk state machine (scheduler RE, Run 15): loadChunk (-> Generated, 2); structure post-process once
// the chunk's 3x3 is Generated (-> 4); decoration once all eight neighbours are at >= 4 plus the DecorationParity
// rule (-> 6); a chunk is final once its whole 3x3 is at >= 6. A delivered chunk must therefore have every chunk
// of its 3x3 decorated, and every decorated chunk needs its 3x3 structure-processed, so for a batch:
//   base terrain   : batch + `ring` 2 (+ the structure pass's own view ring, see makeFeaturesBatchPlan)
//   structure pass : batch + 2 rings
//   decoration     : batch + 1 ring, ascending decorationParityClass, row-major (z, then x) within a class
//   delivery       : the batch only, after ALL decoration (ring chunks are never delivered: their 3x3 is not
//                    fully decorated)
// Neighbour decoration two chunks out (a ring-1 cell's lower-class neighbours in ring 2) is not reproduced; see
// PORTING.md "Run 15" for why and its cost.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include "dh/build/DecorationOrder.h"

namespace dh {

struct ChunkXZ {
    std::int32_t x = 0, z = 0;
    friend bool  operator==(const ChunkXZ& a, const ChunkXZ& b) { return a.x == b.x && a.z == b.z; }
    friend bool  operator<(const ChunkXZ& a, const ChunkXZ& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; }
};

struct FeaturesBatchPlan {
    std::int32_t         minX = 0, minZ = 0, width = 0;
    std::vector<ChunkXZ> base;       ///< base terrain (loadChunk), row-major
    std::vector<ChunkXZ> structures; ///< structure post-process, row-major (one pass per chunk, order-free: see below)
    std::vector<ChunkXZ> decorate;   ///< decoration, in execution order
    std::vector<ChunkXZ> deliver;    ///< delivered after the last decoration, row-major
};

/// All chunks of the square [minX - ring, minX + width + ring) x [minZ - ring, ...), row-major.
inline std::vector<ChunkXZ> squareRing(std::int32_t minX, std::int32_t minZ, std::int32_t width, std::int32_t ring) {
    std::vector<ChunkXZ> out;
    if (width <= 0) return out;
    for (std::int32_t z = minZ - ring; z < minZ + width + ring; ++z)
        for (std::int32_t x = minX - ring; x < minX + width + ring; ++x) out.push_back({x, z});
    return out;
}

/// `structureViewRing`: how many chunks around a structure-processed chunk its structure pass reads or writes (0 when
/// the pass only touches the chunk itself, 1 for a full 3x3 view like vanilla's scheduler waits for).
inline FeaturesBatchPlan makeFeaturesBatchPlan(
    std::int32_t minX,
    std::int32_t minZ,
    std::int32_t width,
    std::int32_t structureViewRing = 0
) {
    FeaturesBatchPlan p;
    p.minX       = minX;
    p.minZ       = minZ;
    p.width      = width;
    p.base       = squareRing(minX, minZ, width, 2 + std::max(0, structureViewRing));
    p.structures = squareRing(minX, minZ, width, 2);
    p.decorate   = squareRing(minX, minZ, width, 1);
    std::stable_sort(p.decorate.begin(), p.decorate.end(), [](const ChunkXZ& a, const ChunkXZ& b) {
        return decorationParityClass(a.x, a.z) < decorationParityClass(b.x, b.z);
    });
    p.deliver = squareRing(minX, minZ, width, 0);
    return p;
}

/// The vanilla dependency checks a plan must pass (host tests). Returns an empty string when it does, else what fails.
inline const char* checkFeaturesBatchPlan(const FeaturesBatchPlan& p, std::int32_t structureViewRing = 0) {
    const std::set<ChunkXZ> base(p.base.begin(), p.base.end());
    const std::set<ChunkXZ> structured(p.structures.begin(), p.structures.end());
    const std::set<ChunkXZ> decorated(p.decorate.begin(), p.decorate.end());
    auto viewIn = [](const std::set<ChunkXZ>& s, const ChunkXZ& c, std::int32_t r) {
        for (std::int32_t dz = -r; dz <= r; ++dz)
            for (std::int32_t dx = -r; dx <= r; ++dx)
                if (!s.count({c.x + dx, c.z + dz})) return false;
        return true;
    };
    for (const auto& c : p.structures)
        if (!viewIn(base, c, structureViewRing)) return "structure-processed chunk without base terrain in its view";
    for (const auto& c : p.decorate)
        if (!viewIn(structured, c, 1)) return "decorated chunk with a neighbour not structure-processed";
    // DecorationParity: a neighbour of a lower class that is decorated at all must be decorated before the chunk.
    for (std::size_t i = 0; i < p.decorate.size(); ++i) {
        const ChunkXZ c = p.decorate[i];
        for (std::size_t j = i + 1; j < p.decorate.size(); ++j) {
            const ChunkXZ n = p.decorate[j];
            if (std::max(std::abs(n.x - c.x), std::abs(n.z - c.z)) != 1) continue;
            if (decorationParityClass(n.x, n.z) < decorationParityClass(c.x, c.z))
                return "lower-class neighbour decorated after the chunk";
        }
    }
    for (const auto& c : p.deliver) {
        if (!viewIn(decorated, c, 1)) return "delivered chunk with an undecorated chunk in its 3x3";
        if (!base.count(c)) return "delivered chunk without base terrain";
    }
    for (const auto& c : p.deliver)
        if (c.x < p.minX || c.x >= p.minX + p.width || c.z < p.minZ || c.z >= p.minZ + p.width)
            return "ring chunk delivered";
    return "";
}

} // namespace dh
