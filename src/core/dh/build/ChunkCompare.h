#pragma once

// Bedrock-port diagnostic (not part of Java DH): how closely a chunk produced by the distant generator matches the
// chunk the game itself generated at the same position. Used by the generator's compare mode
// (DH_BEDROCK_GEN_COMPARE=1, the internal porting notes "Run 13") to measure decoration fidelity in game.

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

    // Staged attribution (compareChunksStaged only; `staged` false = not filled). Every block position is attributed
    // to the LAST generator pass of ours that changed it: structure pass (base -> afterStructures) or decoration pass
    // (afterStructures -> gen, which includes the freeze/snow pass). "Same" = our final block equals the real one.
    bool         staged       = false;
    std::int64_t structGen    = 0; ///< positions our structure pass changed (and decoration did not change again)
    std::int64_t structSame   = 0; ///< ... whose final block equals the real chunk's
    std::int64_t decoGen      = 0; ///< positions our decoration pass changed
    std::int64_t decoSame     = 0; ///< ... whose final block equals the real chunk's
    std::int64_t realOnly     = 0; ///< positions the real chunk changed vs our base terrain that no pass of ours touched
    std::int64_t diffStruct   = 0; ///< differing blocks attributed to our structure pass
    std::int64_t diffDeco     = 0; ///< differing blocks attributed to our decoration pass
    std::int64_t diffUntouched = 0; ///< differing blocks no pass of ours touched (base terrain or real-only blocks)

    /// Tree-column intersection over union, 1 when neither chunk has tree columns.
    double treeIoU() const;
    void   add(const ChunkDiff& o);
    /// One-line summary for the log.
    std::string summary() const;
};

/// Compares two chunks of the same position and height range (`real` = the game's chunk, `gen` = ours).
ChunkDiff compareChunks(const IChunk& real, const IChunk& gen);

/// compareChunks plus the structure / decoration split (Run 15). `base` = our chunk after base terrain only,
/// `afterStructures` = after our structure pass, `gen` = final (after decoration + freeze/snow). All four must be the
/// same position and height range.
ChunkDiff compareChunksStaged(const IChunk& real, const IChunk& base, const IChunk& afterStructures, const IChunk& gen);

} // namespace dh
