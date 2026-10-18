#include "dh/util/Color.h"

#include <cstdio>

namespace dh::color {

std::string toHexString(Argb color) {
    char buf[64];
    std::snprintf(
        buf,
        sizeof buf,
        "A:%x,R:%x,G:%x,B:%x",
        static_cast<unsigned>(getAlpha(color)),
        static_cast<unsigned>(getRed(color)),
        static_cast<unsigned>(getGreen(color)),
        static_cast<unsigned>(getBlue(color))
    );
    return buf;
}

std::string toString(Argb color) {
    return "A:" + std::to_string(getAlpha(color)) + ",R:" + std::to_string(getRed(color))
         + ",G:" + std::to_string(getGreen(color)) + ",B:" + std::to_string(getBlue(color));
}

} // namespace dh::color
