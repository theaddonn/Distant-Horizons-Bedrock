#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/IdMap.h"

namespace dh {

/// Port of the data-model part of Java FullDataSourceV2: a 64x64 grid of columns of
/// full data points. Columns are stored top-down (index 0 = highest bottomY).
/// Merge/downsample logic lives elsewhere (WP-B2). Large (~100 KB): avoid copying.
struct FullDataSource {
    static constexpr int WIDTH        = 64;
    static constexpr int COLUMN_COUNT = WIDTH * WIDTH;

    std::uint64_t pos = 0; ///< raw encoded section pos
    IdMap mapping;
    std::array<std::vector<std::uint64_t>, COLUMN_COUNT> columns;
    std::array<std::int8_t, COLUMN_COUNT> columnGenerationStep{};       ///< GenerationStep values
    std::array<std::int8_t, COLUMN_COUNT> columnWorldCompressionMode{}; ///< WorldCompressionMode values
    bool isEmpty = true;
    /// nullopt = "don't update this value in the DB"
    std::optional<bool> applyToParent, applyToChildren, regenerateLeaf;
    std::int64_t createdUnixMs = 0, lastModifiedUnixMs = 0;

    /// x*64+z. Precondition: 0 <= x,z < WIDTH.
    static constexpr int relativePosToIndex(int x, int z) { return x * WIDTH + z; }
    /// -1 when out of bounds (Java tryGetRelativePosToIndex).
    static constexpr int tryRelativePosToIndex(int x, int z) {
        return (x < 0 || z < 0 || x >= WIDTH || z >= WIDTH) ? -1 : x * WIDTH + z;
    }

    std::vector<std::uint64_t>& column(int x, int z) { return columns[static_cast<std::size_t>(relativePosToIndex(x, z))]; }
    const std::vector<std::uint64_t>& column(int x, int z) const {
        return columns[static_cast<std::size_t>(relativePosToIndex(x, z))];
    }
    /// nullptr when out of bounds (Java tryGetColumnAtRelPos).
    std::vector<std::uint64_t>* tryColumn(int x, int z);
    const std::vector<std::uint64_t>* tryColumn(int x, int z) const;

    std::int8_t genStepAt(int x, int z) const { return columnGenerationStep[static_cast<std::size_t>(relativePosToIndex(x, z))]; }
    std::int8_t compModeAt(int x, int z) const {
        return columnWorldCompressionMode[static_cast<std::size_t>(relativePosToIndex(x, z))];
    }

    /// Replaces the column and sets its generation step / compression mode. Does not touch isEmpty (as Java).
    /// Out-of-bounds x/z is ignored (Java throws).
    void setSingleColumn(std::vector<std::uint64_t> col, int x, int z, std::int8_t genStep, std::int8_t compMode);
    void setSingleColumn(std::vector<std::uint64_t> col, int x, int z, GenerationStep genStep, WorldCompressionMode compMode) {
        setSingleColumn(std::move(col), x, z, static_cast<std::int8_t>(genStep), static_cast<std::int8_t>(compMode));
    }

    /// Java throwIfDataColumnInWrongOrder as a predicate: size >= 2 and first bottomY < last bottomY.
    static bool isColumnInWrongOrder(const std::vector<std::uint64_t>& col);
    /// Java ensureDataColumnOrder: reverses the whole column if it is in the wrong order.
    static void ensureDataColumnOrder(std::vector<std::uint64_t>& col);

    /// Java clearAllNonAdjData: every column outside the edge strip of `dir` becomes {EMPTY_DATA_POINT}.
    void clearAllNonAdjData(AdjDirection dir);

    /// Empty source: empty column lists, steps/modes 0, isEmpty = true, empty mapping with sectionPos = pos.
    static FullDataSource createEmpty(std::uint64_t pos);

    /// Deterministic 64-bit hash replacing Java hashCode. Covers pos, all columns, steps and modes
    /// (NOT the mapping, like Java). Stable across runs/platforms.
    std::uint64_t contentHash() const;
    /// Exact comparison of the same fields contentHash covers.
    bool contentEquals(const FullDataSource& other) const;
};

} // namespace dh
