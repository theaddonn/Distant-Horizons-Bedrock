#pragma once

// Persistent LOD storage (Java: FullDataSourceV2Repo over SQLite; here an
// abstract key-value store so the glue can use ll::data::KeyValueDB/LevelDB
// and tests a MemoryLodStore). WP-D2 implements both; this header only fixes
// the contract.
//
// Record model (one record per section position, Java FullDataSourceV2DTO):
//   data              encoded interior columns (WP-D1 codec output)
//   adjacent[4]       encoded edge strips, indexed by compassIndex(Direction):
//                     0 = North, 1 = South, 2 = West, 3 = East
//   genSteps          per-column GenerationStep bytes (RLE+zstd per D1)
//   compressionModes  per-column world compression mode bytes (RLE+zstd per D1)
//   mapping           block/biome id -> serial table (D1)
//   meta              small fixed fields (format version, compression, flags,
//                     timestamps, checksum)
// Suggested key layout for KV backends: D|det|x|z data, A|dir|pos strips,
// G|pos gen-steps, M|pos meta (plan §2). Parts other than Meta may be loaded
// selectively via get(pos, parts) to avoid reading 4 strips when only Data is
// needed (Java convertResultSetToDto(includeAdjacent)).
//
// Semantics every implementation must honour:
//   * put() writes all supplied parts atomically: a concurrent get() sees either
//     the old record or the new one, never a mix. Parts not in `parts` are
//     left untouched (so flags can be updated without rewriting blobs).
//   * setFlag() is cheap (meta-only write) and never touches blobs.
//   * positionsToPropagate*/positionsToRegen return at most `maxCount`
//     positions ordered by Manhattan distance from the query block position to
//     the section's MIN CORNER block (Java: `(PosX << (6+det))`, see
//     FullDataSourceV2Repo ~:440), ties broken by ascending encoded pos. Only
//     records with the respective flag set are returned.
//   * All position lists are sorted deterministically; never hash order.
//   * Errors are reported through Result/Status; a corrupt record is an error
//     from get(), never a crash.
//   * Thread-safe: called from several core executors concurrently.

#include "dh/util/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace dh {

/// Bitmask selecting record parts for get()/put().
enum class StorePart : std::uint8_t {
    None             = 0,
    Data             = 1 << 0,
    Adjacent         = 1 << 1, ///< all four strips
    GenSteps         = 1 << 2,
    CompressionModes = 1 << 3,
    Mapping          = 1 << 4,
    Meta             = 1 << 5,
    AllBlobs         = Data | Adjacent | GenSteps | CompressionModes | Mapping,
    All              = AllBlobs | Meta,
};

constexpr StorePart operator|(StorePart a, StorePart b) {
    return static_cast<StorePart>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr StorePart operator&(StorePart a, StorePart b) {
    return static_cast<StorePart>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr bool has(StorePart set, StorePart part) { return (set & part) != StorePart::None; }

/// Per-record boolean flags (Java DTO applyToParent / applyToChildren / regenerate).
/// Java stores these as nullable Booleans; the store treats "unset" as false.
/// The in-memory FullDataSource may keep tri-state semantics (WP-B2 merge
/// rules); it is folded to a bit when written here.
enum class StoreFlag : std::uint8_t {
    ApplyToParent   = 1 << 0, ///< this section changed; parent needs re-downsample
    ApplyToChildren = 1 << 1, ///< children need an update from this section
    Regenerate      = 1 << 2, ///< world-gen should regenerate this section
};

constexpr std::uint8_t flagBit(StoreFlag f) { return static_cast<std::uint8_t>(f); }

struct StoreMeta {
    std::uint8_t  dataFormatVersion  = 0; ///< D1 codec version
    std::uint8_t  compressionMode    = 0; ///< D1 Compression enum value
    std::uint8_t  flags              = 0; ///< OR of flagBit(StoreFlag)
    std::uint32_t dataChecksum       = 0; ///< D1-defined checksum of `data`
    std::int64_t  createdMillis      = 0; ///< IClock::nowMillis at first put
    std::int64_t  lastModifiedMillis = 0;

    bool hasFlag(StoreFlag f) const { return (flags & flagBit(f)) != 0; }
    void setFlag(StoreFlag f, bool on) {
        if (on) flags |= flagBit(f);
        else flags &= static_cast<std::uint8_t>(~flagBit(f));
    }
};

struct StoreRecord {
    std::uint64_t                         pos = 0; ///< WP-A encoded DhSectionPos
    std::vector<std::byte>                data;
    std::array<std::vector<std::byte>, 4> adjacent; ///< [compassIndex(Direction)]
    std::vector<std::byte>                genSteps;
    std::vector<std::byte>                compressionModes;
    std::vector<std::byte>                mapping;
    StoreMeta                             meta;
};

struct ILodStore {
    virtual ~ILodStore() = default;

    /// Writes the selected `parts` of `record` atomically. Creates the record
    /// if absent (then all parts should be supplied). Meta.lastModifiedMillis
    /// is taken from `record` as given (callers stamp it from IClock).
    virtual Status put(const StoreRecord& record, StorePart parts = StorePart::All) = 0;

    /// Loads the selected parts; unselected blobs are left empty.
    /// nullopt if the position has no record. Corrupt -> error.
    virtual Result<std::optional<StoreRecord>> get(std::uint64_t pos, StorePart parts = StorePart::All) = 0;

    virtual Result<bool> exists(std::uint64_t pos) = 0;

    /// Removes the whole record. Success if absent.
    virtual Status erase(std::uint64_t pos) = 0;

    /// Meta-only flag update. NotFound if the record does not exist.
    virtual Status setFlag(std::uint64_t pos, StoreFlag flag, bool value) = 0;

    /// Positions with ApplyToParent, nearest first (see header comment).
    virtual Result<std::vector<std::uint64_t>>
    positionsToPropagateUp(std::int32_t blockX, std::int32_t blockZ, std::size_t maxCount) = 0;

    /// Positions with ApplyToChildren, nearest first.
    virtual Result<std::vector<std::uint64_t>>
    positionsToPropagateDown(std::int32_t blockX, std::int32_t blockZ, std::size_t maxCount) = 0;

    /// Positions with Regenerate whose min corner is within `maxBlockDistance`
    /// (Manhattan) of the query, nearest first.
    virtual Result<std::vector<std::uint64_t>>
    positionsToRegen(std::int32_t blockX, std::int32_t blockZ, std::int32_t maxBlockDistance, std::size_t maxCount) = 0;

    /// Number of Regenerate-flagged records within `maxBlockDistance` (negative =
    /// unlimited). NOTE: Java's getRegenChunkCount returns a 2^(detail+6)-weighted
    /// sum (chunk count); this is a plain record count, callers weight if needed.
    virtual Result<std::size_t> regenRecordCount(std::int32_t blockX, std::int32_t blockZ, std::int32_t maxBlockDistance) = 0;

    /// lastModifiedMillis for `pos`, nullopt if absent.
    virtual Result<std::optional<std::int64_t>> timestamp(std::uint64_t pos) = 0;

    /// (pos, lastModifiedMillis) for every record at `detailLevel` whose section
    /// X/Z (at that detail) lie in [startX, endX) x [startZ, endZ): start inclusive, END EXCLUSIVE
    /// (Java FullDataSourceV2Repo.getTimestampsForRange binds `end - 1` into a BETWEEN).
    /// Sorted by pos ascending.
    virtual Result<std::vector<std::pair<std::uint64_t, std::int64_t>>> timestamps(
        std::uint8_t detailLevel,
        std::int32_t startX,
        std::int32_t startZ,
        std::int32_t endX,
        std::int32_t endZ
    ) = 0;

    /// Every stored position, sorted ascending.
    virtual Result<std::vector<std::uint64_t>> allPositions() = 0;

    /// Flushes pending writes to durable storage (no-op for memory stores).
    virtual Status flush() = 0;
};

} // namespace dh
