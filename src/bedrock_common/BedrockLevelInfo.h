#pragma once

#include "dh/platform/TableClientLevel.h"

class Dimension;

namespace dh::bedrock {

/// Height range (mHeightRange, max exclusive), mHasCeiling, mHasSkylight, dimension id
/// (0 overworld, 1 nether, 2 the end; others use the dimension's own name lowercased).
dh::LevelInfo levelInfoOf(const Dimension& dimension);

} // namespace dh::bedrock
