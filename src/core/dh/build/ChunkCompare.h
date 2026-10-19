#pragma once

// Bedrock-port diagnostic (not part of Java DH): how closely a chunk produced by the distant generator matches the
// chunk the game itself generated at the same position. Used by the generator's compare mode
// (DH_BEDROCK_GEN_COMPARE=1, docs/PORTING.md "Run 13") to measure decoration fidelity in game.

#include <cstdint>
#include <string>

#include "dh/platform/IChunk.h"

namespace dh {

struct ChunkDiff {
    int columns      = 0; ///< 256
    int sameTopY     = 0; ///< columns whose highest non-air block is at the same y
    int sameTopBlock = 0; ///< ... and is the same block state
    // columns whose highest non-air block is a tree part (material Leaves or Wood)
    int treeReal = 0, treeGen = 0, treeBoth = 0;
    // columns whose highest non-air block is snow (material Snow) / ice (isIce)
    int snowReal = 0, snowGen = 0;
    int iceReal = 0, iceGen = 0;
    /// block positions (whole build range) whose block state differs
    std::int64_t diffBlocks = 0;

    /// Tree-column intersection over union, 1 when neither chunk has tree columns.
    double treeIoU() const;
    void   add(const ChunkDiff& o);
    /// One-line summary for the log.
    std::string summary() const;
};

/// Compares two chunks of the same position and height range (`real` = the game's chunk, `gen` = ours).
ChunkDiff compareChunks(const IChunk& real, const IChunk& gen);

} // namespace dh
