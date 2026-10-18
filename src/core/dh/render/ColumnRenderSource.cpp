#include "dh/render/ColumnRenderSource.h"

#include <algorithm>

#include "dh/render/RenderDataPoint.h"

namespace dh {

bool ColumnRenderView::populate(std::span<std::uint64_t> d, std::span<std::uint64_t> v, int sz, int offset,
                                int voidOffset, int maxSlices) {
    data                  = d;
    voidData              = v;
    size                  = sz;
    dataOffset            = offset;
    voidDataOffset        = voidOffset;
    maxVerticalSliceCount = maxSlices;

    // Java: throws if the arrays are shorter than the offsets (offset == length is allowed)
    return static_cast<int>(data.size()) >= dataOffset && static_cast<int>(voidData.size()) >= voidDataOffset;
}

void ColumnRenderView::clear() {
    data                  = {};
    voidData              = {};
    size                  = 0;
    dataOffset            = 0;
    voidDataOffset        = 0;
    maxVerticalSliceCount = 0;
}

ColumnRenderView ColumnRenderView::subView(int dataIndexStart, int dataCount) const {
    ColumnRenderView v;
    v.data                  = data;
    v.voidData              = voidData;
    v.size                  = dataCount * maxVerticalSliceCount;
    v.dataOffset            = dataOffset + dataIndexStart * maxVerticalSliceCount;
    v.voidDataOffset        = voidDataOffset + dataIndexStart;
    v.maxVerticalSliceCount = maxVerticalSliceCount;
    return v;
}

bool ColumnRenderView::copyFrom(const ColumnRenderView& source, int outputDataIndexOffset) {
    if (source.maxVerticalSliceCount > maxVerticalSliceCount) {
        return false; // "source verticalSize must be <= self's verticalSize to copy"
    }
    if (source.subViewCount() + outputDataIndexOffset > subViewCount()) {
        return false; // "dataIndexStart + source.dataCount() must be <= self.dataCount() to copy"
    }

    if (source.maxVerticalSliceCount != maxVerticalSliceCount) {
        for (int i = 0; i < source.subViewCount(); ++i) {
            const int outputOffset = dataOffset + outputDataIndexOffset * maxVerticalSliceCount + i * maxVerticalSliceCount;
            const ColumnRenderView sub = source.subView(i, 1);
            auto*                  dst = data.data() + outputOffset;
            std::copy_n(sub.data.data() + sub.dataOffset, source.maxVerticalSliceCount, dst);
            std::fill(dst + source.maxVerticalSliceCount, dst + maxVerticalSliceCount, std::uint64_t{0});
        }
    } else {
        std::copy_n(source.data.data() + source.dataOffset, source.size,
                    data.data() + dataOffset + outputDataIndexOffset * maxVerticalSliceCount);
    }
    return true;
}

bool ColumnRenderView::changeVerticalSizeFrom(const ColumnRenderView& source) {
    if (subViewCount() != source.subViewCount()) {
        return false; // "Cannot copy and resize to views with different dataCounts"
    }

    if (maxVerticalSliceCount >= source.maxVerticalSliceCount) {
        if (!copyFrom(source, 0)) {
            return false;
        }
    } else {
        // TODO(WP-E1): per sub-view mergeMultiData via RenderDataPointReducingList
        // (target==1 -> reduceToOne, otherwise populate/reduce/copyTo).
        return false;
    }

    // copy over the void data too
    setVoid(source.getVoid());
    return true;
}

ColumnRenderSource ColumnRenderSource::createEmpty(std::uint64_t pos, int maxVerticalSlices, int yOffset) {
    ColumnRenderSource s;
    s.pos               = pos;
    s.maxVerticalSlices = maxVerticalSlices;
    s.yOffset           = yOffset;
    s.data.assign(static_cast<std::size_t>(WIDTH * WIDTH * std::max(maxVerticalSlices, 0)), rdp::EMPTY_DATA);
    s.voidData.assign(static_cast<std::size_t>(WIDTH * WIDTH), rdp::EMPTY_DATA);
    return s;
}

std::span<std::uint64_t> ColumnRenderSource::column(int x, int z) {
    if (!inBounds(x, z) || maxVerticalSlices <= 0) {
        return {};
    }
    return std::span<std::uint64_t>(data).subspan(static_cast<std::size_t>(dataIndex(x, z, maxVerticalSlices, 0)),
                                                  static_cast<std::size_t>(maxVerticalSlices));
}

std::span<const std::uint64_t> ColumnRenderSource::column(int x, int z) const {
    if (!inBounds(x, z) || maxVerticalSlices <= 0) {
        return {};
    }
    return std::span<const std::uint64_t>(data).subspan(static_cast<std::size_t>(dataIndex(x, z, maxVerticalSlices, 0)),
                                                        static_cast<std::size_t>(maxVerticalSlices));
}

bool ColumnRenderSource::populateColumnView(ColumnRenderView& view, int x, int z) {
    if (!inBounds(x, z)) {
        return false;
    }
    const int dataOffset     = dataIndex(x, z, maxVerticalSlices, 0);
    const int voidDataOffset = voidIndex(x, z);
    // Java: offset >= size is an error, so an empty (0-slice) source can never produce a view.
    if (dataOffset >= static_cast<int>(data.size()) || voidDataOffset >= static_cast<int>(voidData.size())) {
        return false;
    }
    // Java passes maxVerticalSliceCount as both the view size and the slice count.
    return view.populate(data, voidData, maxVerticalSlices, dataOffset, voidDataOffset, maxVerticalSlices);
}

bool ColumnRenderSource::hasNonVoidDataPoints() {
    if (isEmpty) {
        return false;
    }
    ColumnRenderView view;
    for (int x = 0; x < WIDTH; ++x) {
        for (int z = 0; z < WIDTH; ++z) {
            if (!populateColumnView(view, x, z)) {
                return false;
            }
            for (int i = 0; i < view.size; ++i) {
                if (!rdp::hasZeroHeight(view.get(i))) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace dh
