#include "dh/tree/QuadTree.h"

namespace dh::tree::detail {

// QuadTree.DoSquaresOverlap. Java names the locals rect[angle] but only ever passes squares. The coordinate sums
// are int additions (wrapping) that are then widened to float, and the comparisons are done on floats.
bool doSquaresOverlap(sec::Pos square1Min, std::int32_t square1Width, sec::Pos square2Min, std::int32_t square2Width) {
    const float rect1MinX = static_cast<float>(sec::x(square1Min));
    const float rect1MaxX = static_cast<float>(wadd(sec::x(square1Min), square1Width));
    const float rect1MinZ = static_cast<float>(sec::z(square1Min));
    const float rect1MaxZ = static_cast<float>(wadd(sec::z(square1Min), square1Width));

    const float rect2MinX = static_cast<float>(sec::x(square2Min));
    const float rect2MaxX = static_cast<float>(wadd(sec::x(square2Min), square2Width));
    const float rect2MinZ = static_cast<float>(sec::z(square2Min));
    const float rect2MaxZ = static_cast<float>(wadd(sec::z(square2Min), square2Width));

    return rect1MinX < rect2MaxX && rect1MaxX > rect2MinX && rect1MinZ < rect2MaxZ && rect1MaxZ > rect2MinZ;
}

} // namespace dh::tree::detail
