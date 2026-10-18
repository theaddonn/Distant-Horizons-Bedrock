#pragma once

#include <cstdint>

/// Port of Java FullDataPointUtil. Bit layout (trust the offsets, the Java diagram is wrong):
///   id 31 bits @0 (bit 31 unused), height 12 @32, relMinY 12 @44, skyLight 4 @56, blockLight 4 @60.
/// Like Java, nothing is validated and several setters/encode do NOT mask their input
/// (an out-of-range value bleeds into the neighbouring field). That is kept on purpose.
namespace dh::fdp {

constexpr std::uint64_t ID_MASK     = 0x7FFFFFFFull; // Integer.MAX_VALUE
constexpr std::uint64_t HEIGHT_MASK = 0xFFF;
constexpr std::uint64_t MIN_Y_MASK  = 0xFFF;
constexpr std::uint64_t SKY_MASK    = 0xF;
constexpr std::uint64_t BLOCK_MASK  = 0xF;
constexpr int ID_OFFSET             = 0;
constexpr int HEIGHT_OFFSET         = 32;
constexpr int MIN_Y_OFFSET          = 44;
constexpr int SKY_OFFSET            = 56;
constexpr int BLOCK_OFFSET          = 60;

constexpr std::uint64_t EMPTY_DATA_POINT = 0;

constexpr bool isEmpty(std::uint64_t d) { return d == EMPTY_DATA_POINT; }

namespace detail {
/// Java int -> long widening (sign extension).
constexpr std::uint64_t widen(int v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); }
} // namespace detail

/// Java encode (validation omitted). id/height/relMinY are masked, the light bytes are not.
constexpr std::uint64_t encode(int id, int height, int relMinY, std::uint8_t blockLight, std::uint8_t skyLight) {
    std::uint64_t data = 0;
    data |= detail::widen(id) & ID_MASK;
    data |= (detail::widen(height) & HEIGHT_MASK) << HEIGHT_OFFSET;
    data |= (detail::widen(relMinY) & MIN_Y_MASK) << MIN_Y_OFFSET;
    data |= static_cast<std::uint64_t>(blockLight) << BLOCK_OFFSET;
    data |= static_cast<std::uint64_t>(skyLight) << SKY_OFFSET;
    return data;
}

constexpr int id(std::uint64_t d) { return static_cast<int>(d & ID_MASK); }
constexpr int height(std::uint64_t d) { return static_cast<int>((d >> HEIGHT_OFFSET) & HEIGHT_MASK); }
/// Java getBottomY
constexpr int minY(std::uint64_t d) { return static_cast<int>((d >> MIN_Y_OFFSET) & MIN_Y_MASK); }
constexpr std::uint8_t blockLight(std::uint64_t d) { return static_cast<std::uint8_t>((d >> BLOCK_OFFSET) & BLOCK_MASK); }
constexpr std::uint8_t skyLight(std::uint64_t d) { return static_cast<std::uint8_t>((d >> SKY_OFFSET) & SKY_MASK); }

/// Java: clears only the 31 id bits, then ORs the sign-extended id (a negative id corrupts the upper fields).
constexpr std::uint64_t setId(std::uint64_t d, int newId) { return (d & ~ID_MASK) | detail::widen(newId); }
/// Java: new value is not masked.
constexpr std::uint64_t setHeight(std::uint64_t d, int h) {
    return (d & ~(HEIGHT_MASK << HEIGHT_OFFSET)) | (detail::widen(h) << HEIGHT_OFFSET);
}
constexpr std::uint64_t setMinY(std::uint64_t d, int y) {
    return (d & ~(MIN_Y_MASK << MIN_Y_OFFSET)) | (detail::widen(y) << MIN_Y_OFFSET);
}
/// Java: value not masked (bits >= 4 of the byte are shifted out at offset 60).
constexpr std::uint64_t setBlockLight(std::uint64_t d, std::uint8_t v) {
    return (d & ~(BLOCK_MASK << BLOCK_OFFSET)) | (static_cast<std::uint64_t>(v) << BLOCK_OFFSET);
}
/// Java: value not masked; values > 15 bleed into the blockLight nibble.
constexpr std::uint64_t setSkyLight(std::uint64_t d, int v) {
    return (d & ~(SKY_MASK << SKY_OFFSET)) | (detail::widen(v) << SKY_OFFSET);
}

/// Java remap helper: replaces the id via a lookup table. Caller guarantees id < table size.
constexpr std::uint64_t remapId(std::uint64_t d, const int* newIdByOldId) { return setId(d, newIdByOldId[id(d)]); }

} // namespace dh::fdp
