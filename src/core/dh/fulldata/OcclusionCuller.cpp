#include "dh/fulldata/OcclusionCuller.h"

#include "dh/fulldata/FullDataPoint.h"

namespace dh {

namespace {
constexpr int BLOCK_FULLY_OPAQUE = 16;

/// Java would throw on an unknown id; an unknown block is treated as translucent (never culled).
bool isTranslucent(const FullDataSource& source, std::uint64_t point) {
    const BlockInfo* b = source.mapping.block(fdp::id(point));
    if (!b) return true;
    return b->opacity < BLOCK_FULLY_OPAQUE;
}
} // namespace

int checkOcclusion(
    const FullDataSource&             source,
    std::uint64_t                     centerPoint,
    const std::vector<std::uint64_t>& adjacentColumn,
    int                               adjacentIndex
) {
    if (adjacentColumn.empty()) return ~adjacentIndex;
    if (adjacentColumn.size() == 1 && adjacentColumn[0] == fdp::EMPTY_DATA_POINT) return ~adjacentIndex;

    const int bottomOfCenter = fdp::minY(centerPoint);
    const int topOfCenter    = bottomOfCenter + fdp::height(centerPoint);
    for (; adjacentIndex >= 0; adjacentIndex--) {
        const std::uint64_t adjacentPoint = adjacentColumn[static_cast<std::size_t>(adjacentIndex)];
        const int           topOfAdjacent = fdp::minY(adjacentPoint) + fdp::height(adjacentPoint);
        if (topOfAdjacent <= bottomOfCenter) {
            continue;
        } else if (isTranslucent(source, adjacentPoint)) {
            return ~adjacentIndex;
        } else if (topOfAdjacent >= topOfCenter) {
            return adjacentIndex;
        }
    }
    return ~adjacentIndex;
}

void cullHiddenDatapointsInColumn(FullDataSource& dataSource, int relX, int relZ) {
    auto&       centerColumn = dataSource.column(relX, relZ);
    const auto* posXColumn   = dataSource.tryColumn(relX + 1, relZ);
    const auto* negXColumn   = dataSource.tryColumn(relX - 1, relZ);
    const auto* posZColumn   = dataSource.tryColumn(relX, relZ + 1);
    const auto* negZColumn   = dataSource.tryColumn(relX, relZ - 1);

    if (!posXColumn || posXColumn->empty() || !negXColumn || negXColumn->empty() || !posZColumn || posZColumn->empty()
        || !negZColumn || negZColumn->empty()) {
        return;
    }

    int centerIndex = static_cast<int>(centerColumn.size()) - 1;
    int posXIndex   = static_cast<int>(posXColumn->size()) - 1;
    int negXIndex   = static_cast<int>(negXColumn->size()) - 1;
    int posZIndex   = static_cast<int>(posZColumn->size()) - 1;
    int negZIndex   = static_cast<int>(negZColumn->size()) - 1;

    auto at = [](const std::vector<std::uint64_t>& c, int i) { return c[static_cast<std::size_t>(i)]; };

    for (; centerIndex >= 0; centerIndex--) {
        const std::uint64_t currentPoint = at(centerColumn, centerIndex);

        if (isTranslucent(dataSource, currentPoint)) continue;
        // the top segment is never culled
        if (centerIndex == 0 || isTranslucent(dataSource, at(centerColumn, centerIndex - 1))) continue;
        if (centerIndex + 1 < static_cast<int>(centerColumn.size())
            && isTranslucent(dataSource, at(centerColumn, centerIndex + 1))) {
            continue;
        }
        // the lowest segment is never culled
        if (centerIndex + 1 == static_cast<int>(centerColumn.size())) continue;

        posXIndex = checkOcclusion(dataSource, currentPoint, *posXColumn, posXIndex);
        if (posXIndex < 0) {
            posXIndex = ~posXIndex;
            continue;
        }
        negXIndex = checkOcclusion(dataSource, currentPoint, *negXColumn, negXIndex);
        if (negXIndex < 0) {
            negXIndex = ~negXIndex;
            continue;
        }
        posZIndex = checkOcclusion(dataSource, currentPoint, *posZColumn, posZIndex);
        if (posZIndex < 0) {
            posZIndex = ~posZIndex;
            continue;
        }
        negZIndex = checkOcclusion(dataSource, currentPoint, *negZColumn, negZIndex);
        if (negZIndex < 0) {
            negZIndex = ~negZIndex;
            continue;
        }

        // fully surrounded: remove, and let the datapoint above absorb its range
        centerColumn.erase(centerColumn.begin() + centerIndex);
        std::uint64_t above = at(centerColumn, centerIndex - 1);
        above               = fdp::setMinY(above, fdp::minY(currentPoint));
        above               = fdp::setHeight(above, fdp::height(currentPoint) + fdp::height(above));
        centerColumn[static_cast<std::size_t>(centerIndex - 1)] = above;
    }
}

} // namespace dh
