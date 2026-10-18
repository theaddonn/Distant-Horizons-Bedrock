#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dh/render/Enums.h"

namespace dh {

/// Port of Java ColumnRenderView: a window onto one or more columns of a ColumnRenderSource.
/// Non-owning; the parent source must outlive it. Not thread safe.
///
/// Not ported: the Java object pool (`getPooled`/`close`) and the texture palette
/// (`textureSetPaletteIndices`/`texturePalette`) of ColumnRenderSource.
struct ColumnRenderView {
    std::span<std::uint64_t> data;
    std::span<std::uint64_t> voidData;

    /// Number of datapoints represented (<= data capacity from dataOffset).
    int size = 0;
    /// Slices per column of the parent source; may be 0 for an empty source.
    int maxVerticalSliceCount = 0;
    int dataOffset            = 0;
    int voidDataOffset        = 0;

    /// Java populate. Returns false (view left unusable) when an offset is past the end of its array.
    bool populate(
        std::span<std::uint64_t> data,
        std::span<std::uint64_t> voidData,
        int                      size,
        int                      offset,
        int                      voidDataOffset,
        int                      maxVerticalSliceCount
    );
    void clear();

    std::uint64_t get(int index) const { return data[static_cast<std::size_t>(index + dataOffset)]; }
    void          set(int index, std::uint64_t value) { data[static_cast<std::size_t>(index + dataOffset)] = value; }

    /// The void datapoint below this column.
    std::uint64_t getVoid() const { return voidData[static_cast<std::size_t>(voidDataOffset)]; }
    void          setVoid(std::uint64_t value) { voidData[static_cast<std::size_t>(voidDataOffset)] = value; }

    /// The datapoints [0, size) of this view.
    std::span<std::uint64_t> points() const {
        return data.subspan(static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(size));
    }

    /// Java subView: `dataCount` columns starting at column `dataIndexStart`.
    /// Returns an empty view (size 0, no data) when the window starts past the end of the arrays.
    ColumnRenderView subView(int dataIndexStart, int dataCount) const;
    /// Number of columns covered (size / maxVerticalSliceCount, 0 if maxVerticalSliceCount == 0).
    int subViewCount() const { return maxVerticalSliceCount != 0 ? size / maxVerticalSliceCount : 0; }

    /// Java copyFrom (private there): copies `source` into this view starting at column
    /// `outputDataIndexOffset`, zero-padding when source has fewer slices per column.
    /// Returns false (nothing written) when Java would throw IllegalArgumentException.
    bool copyFrom(const ColumnRenderView& source, int outputDataIndexOffset);

    /// Java changeVerticalSizeFrom. Returns false where Java throws IllegalArgumentException.
    /// TODO(WP-E1): the reducing path (this.maxVerticalSliceCount < source.maxVerticalSliceCount)
    /// needs RenderDataPointReducingList and is not implemented: it returns false and writes nothing.
    bool changeVerticalSizeFrom(const ColumnRenderView& source);
};

/// Port of Java ColumnRenderSource (data container only).
struct ColumnRenderSource {
    /// measured in data columns
    static constexpr int WIDTH = 64;

    /// Opaque section position (DhSectionPos packed long; WP-A provides the helpers).
    std::uint64_t pos = 0;
    /// 0 if an empty data source was created.
    int maxVerticalSlices = 0;
    int yOffset           = 0;

    /// WIDTH * WIDTH * maxVerticalSlices datapoints, index = x*WIDTH*slices + z*slices + i.
    std::vector<std::uint64_t> data;
    /// WIDTH * WIDTH void datapoints, index = x*WIDTH + z: the air below columns that
    /// do not reach the world's minimum height (keeps lighting correct).
    std::vector<std::uint64_t> voidData;
    bool                       isEmpty = true;

    /// Java createEmpty.
    static ColumnRenderSource createEmpty(std::uint64_t pos, int maxVerticalSlices, int yOffset);

    /// Java calculateMaxNumberOfVerticalSlicesAtDetailLevel.
    static int maxSlices(VerticalQuality q, int dataDetail) { return maxVerticalData(q, dataDetail); }

    static int dataIndex(int x, int z, int slices, int verticalIndex) {
        return x * WIDTH * slices + z * slices + verticalIndex;
    }
    static int voidIndex(int x, int z) { return x * WIDTH + z; }

    static bool inBounds(int x, int z) { return x >= 0 && x < WIDTH && z >= 0 && z < WIDTH; }

    std::uint64_t dataPoint(int x, int z, int verticalIndex) const {
        return data[static_cast<std::size_t>(dataIndex(x, z, maxVerticalSlices, verticalIndex))];
    }

    /// All slices of column (x, z). Empty span if the source has no slices or x/z are out of range.
    std::span<std::uint64_t>       column(int x, int z);
    std::span<const std::uint64_t> column(int x, int z) const;

    /// Java populateColumnView. Returns false where Java throws IllegalArgumentException
    /// (x/z out of range, or an offset past the end of the arrays, e.g. an empty source).
    bool populateColumnView(ColumnRenderView& view, int x, int z);

    void markNotEmpty() { isEmpty = false; }

    /// Java hasNonVoidDataPoints (debug helper).
    bool hasNonVoidDataPoints();
};

} // namespace dh
