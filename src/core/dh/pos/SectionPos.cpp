#include "dh/pos/SectionPos.h"

namespace dh::sec {

std::string toString(Pos pos) {
    return std::to_string(static_cast<int>(detail(pos))) + "*" + std::to_string(x(pos)) + "," + std::to_string(z(pos));
}

} // namespace dh::sec
