#include "bedrock_common/BedrockLevelInfo.h"

#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <cctype>

namespace dh::bedrock {

dh::LevelInfo levelInfoOf(const Dimension& d) {
    dh::LevelInfo li;
    li.minHeight   = d.mHeightRange->mMin;
    li.maxHeight   = d.mHeightRange->mMax;
    li.hasCeiling  = d.mHasCeiling;
    li.hasSkyLight = d.mHasSkylight;
    const int id   = d.mId->mValue;
    li.isEnd       = id == 2;
    switch (id) {
    case 0:
        li.dimensionName = "minecraft:overworld";
        break;
    case 1:
        li.dimensionName = "minecraft:the_nether";
        break;
    case 2:
        li.dimensionName = "minecraft:the_end";
        break;
    default: {
        std::string n = d.mName.get();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        li.dimensionName = n.find(':') == std::string::npos ? "minecraft:" + n : n;
    }
    }
    return li;
}

} // namespace dh::bedrock
