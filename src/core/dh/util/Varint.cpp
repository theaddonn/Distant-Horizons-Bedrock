#include "dh/util/Varint.h"

namespace dh::varint {

std::uint32_t zigzag(std::int32_t n) {
    const std::uint32_t u = static_cast<std::uint32_t>(n);
    return (u << 1) ^ static_cast<std::uint32_t>(n >> 31);
}

std::int32_t unzigzag(std::uint32_t n) { return static_cast<std::int32_t>((n >> 1) ^ (0u - (n & 1u))); }

bool writeVarint(std::vector<std::byte>& out, std::uint32_t value) {
    if (value > 0x7FFFFFFFu) {
        return false;
    }
    while (value >= 128) {
        out.push_back(static_cast<std::byte>((value | 128u) & 0xFFu));
        value >>= 7;
    }
    out.push_back(static_cast<std::byte>(value));
    return true;
}

std::optional<std::uint32_t> readVarint(std::span<const std::byte> in, std::size_t& offset) {
    std::uint32_t value = 0;
    std::uint32_t shift = 0;
    std::size_t   pos   = offset;
    std::uint32_t b;
    do {
        if (shift >= 32) {
            return std::nullopt;
        }
        if (pos >= in.size()) {
            return std::nullopt;
        }
        b      = static_cast<std::uint32_t>(in[pos++]);
        value |= (b & 127u) << shift;
        shift += 7;
    } while ((b & 128u) != 0);
    offset = pos;
    return value;
}

} // namespace dh::varint
