#pragma once

// Binary codec for FullDataSource (WP-D1): the on-disk record (LevelDB parts D|A|G|M)
// and the DH network payload. Bedrock-optimised format, NOT byte compatible with
// Java; keeps Java's columnar structure. Exact byte layout: docs/FORMAT.md.
//
// All functions are exception-free; malformed input yields an Error (Corrupt /
// Unsupported), never UB. Thread-safe (no shared state), except that decode calls
// into the caller's IBlockRegistry.

#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/Direction.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILodStore.h"
#include "dh/util/ByteIO.h"
#include "dh/util/Result.h"

#include <cstddef>
#include <cstdint>

namespace dh {

enum class Compression : std::uint8_t {
    None = 0,
    Zstd = 1, ///< TODO(WP-D1b): id and framing are reserved; encode/decode return Unsupported until zstd is vendored
};

struct CodecOptions {
    Compression compression = Compression::None;
    int         zstdLevel   = 3;
};

constexpr std::uint8_t kCodecFormatVersion = 1;
/// Upper bound for any single blob / decompressed payload / wire blob.
constexpr std::size_t kCodecMaxBlobBytes = 64u * 1024u * 1024u;

/// Encodes all record parts. Fails with InvalidArgument when the source cannot be
/// represented (data point id outside the mapping, column longer than 4096, ...).
Result<StoreRecord> encode(const FullDataSource& src, const CodecOptions& opt = {});

/// Full decode. Requires Data, Adjacent[4], GenSteps, CompressionModes, Mapping and Meta
/// (the data checksum is verified). Unknown block/biome serials fall back through
/// registry.resolveBlockOrAir / resolveBiomeOrDefault with a warning.
Result<FullDataSource> decode(const StoreRecord& rec, IBlockRegistry& registry);

/// Decodes only the edge strip `dir` (North/South/West/East) plus mapping/genSteps/modes/flags;
/// needs neither `data` nor the other strips. All other columns are {EMPTY_DATA_POINT}
/// (== clearAllNonAdjData of the full decode). The data checksum is NOT verified.
Result<FullDataSource> decodeAdjacentOnly(const StoreRecord& rec, Direction dir, IBlockRegistry& registry);

/// The checksum encode() stores in meta.dataChecksum (FNV-1a 32 over the uncompressed
/// payloads of data, the four adjacent blobs, genSteps, compressionModes and mapping, each prefixed by its u32 LE
/// length).
Result<std::uint32_t> computeDataChecksum(const StoreRecord& rec);

/// Wire form (big-endian envelope, little-endian blobs inside).
void                writeRecord(ByteWriter& out, const StoreRecord& rec);
Result<StoreRecord> readRecord(ByteReader& in);

} // namespace dh
