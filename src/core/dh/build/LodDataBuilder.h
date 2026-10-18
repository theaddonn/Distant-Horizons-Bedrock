#pragma once

// Port of Java LodDataBuilder.createFromChunk (coreSubProjects/core/.../dataObjects/transformers/
// LodDataBuilder.java:56-275) and its private helper getXOrZSectionPosFromChunkPos (:507).
// The API-chunk entry points (createFromApiChunkData & co) are out of scope.

#include <cstdint>
#include <optional>

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IChunk.h"

namespace dh {

struct BuildOptions {
    /// Java Config.Common.LodBuilding.worldCompression; only stored per column (:253).
    WorldCompressionMode worldCompression = WorldCompressionMode::MergeSameBlocks;
};

/// Java LodDataBuilder.createFromChunk. Fills the 16x16 columns of the detail-6 section that contain
/// `chunk`; every other column stays empty. nullopt when Java would return null (DataCorruptedException,
/// i.e. a produced column is in the wrong order).
///
/// Requires chunk.isDhBlockLightingCorrect() (Java LodUtil.assertTrue, :61; DH_ASSERT here).
///
/// Deliberate production-build choices (RUN_RELATIVE_POS_INDEX_VALIDATION == false, IS_DEV_BUILD off):
/// reads outside the build range follow the IChunk contract (air / light 15 above, 0 below) instead of
/// throwing, so the "climb" at :178 ends at y == exclusiveMaxBuildHeight for a column that is solid up to
/// the build limit (the Java catch-and-clamp branch is only reachable in dev builds).
/// The API chunk-processing event override (:205-221) is not ported.
std::optional<FullDataSource> createFromChunk(const IChunk& chunk, const BuildOptions& options);

/// Detail-6 section pos containing the chunk (Java getXOrZSectionPosFromChunkPos + DhSectionPos.encode).
std::uint64_t sectionPosForChunk(std::int32_t chunkX, std::int32_t chunkZ);

/// Column offset of the chunk inside its section: (chunkCoord & 3) * 16 (Java :105-117), 0/16/32/48.
int chunkOffsetInSection(std::int32_t chunkCoord);

} // namespace dh
