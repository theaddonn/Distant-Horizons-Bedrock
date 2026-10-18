#pragma once

#include <cstdint>
#include <vector>

#include "dh/fulldata/FullDataSource.h"

/// Port of Java FullDataOcclusionCuller.
namespace dh {

/// Removes datapoints that are completely surrounded by opaque neighbours (the datapoint above
/// absorbs the removed one). Precondition: 0 <= relX, relZ < 64.
void cullHiddenDatapointsInColumn(FullDataSource& dataSource, int relX, int relZ);

/// Java checkOcclusion. Covered: returns the (non-negative) index of the covering segment.
/// Not covered: returns ~index (negative). Quirk kept from Java: when the scan runs off the
/// bottom, adjacentIndex is -1 and ~(-1) == 0 is returned, which reads as "covered".
int checkOcclusion(
    const FullDataSource&             source,
    std::uint64_t                     centerPoint,
    const std::vector<std::uint64_t>& adjacentColumn,
    int                               adjacentIndex
);

} // namespace dh
