#pragma once

// Constants from core/util/LodUtil.java and FullDataSourceV2 that the position
// classes depend on.

#include <cstdint>

namespace dh::lod {

/// 512 blocks wide.
inline constexpr std::int8_t REGION_DETAIL_LEVEL = 9;
/// 16 blocks wide.
inline constexpr std::int8_t CHUNK_DETAIL_LEVEL = 4;
/// 1 block wide.
inline constexpr std::int8_t BLOCK_DETAIL_LEVEL = 0;

inline constexpr std::int16_t REGION_WIDTH           = 512;
inline constexpr std::int16_t CHUNK_WIDTH            = 16;
inline constexpr std::int32_t REGION_WIDTH_IN_CHUNKS = REGION_WIDTH / CHUNK_WIDTH;

/// FullDataSourceV2.WIDTH: LOD columns per section side.
inline constexpr std::int32_t FULL_DATA_WIDTH = 64;
/// FullDataSourceV2.NUMB_OF_CHUNKS_WIDE.
inline constexpr std::int32_t NUMB_OF_CHUNKS_WIDE = FULL_DATA_WIDTH / CHUNK_WIDTH;

} // namespace dh::lod
