#pragma once

// Port of sql/dto/util/VarintUtil.java (LEB128-style varint over zigzag values).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dh::varint {

/// VarintUtil.zigzagEncode: (n << 1) ^ (n >> 31).
std::uint32_t zigzag(std::int32_t n);
/// VarintUtil.zigzagDecode: (n >>> 1) ^ -(n & 1).
std::int32_t unzigzag(std::uint32_t n);

/// VarintUtil.writeVarint. Java rejects negative ints ("varint only accepts positive
/// values"), i.e. here anything above INT32_MAX. Returns false (and writes nothing)
/// in that case. Note this means zigzag values for |n| >= 2^30 cannot be written.
bool writeVarint(std::vector<std::byte>& out, std::uint32_t value);

/// VarintUtil.readVarint: reads at most 5 bytes (shift cap 32). Advances `offset`
/// on success; returns nullopt (offset untouched) on truncated input or when the
/// varint continues past shift 32 ("invalid varint"). The result is the raw
/// 32-bit pattern Java would return (high bits of a 5th byte are dropped).
std::optional<std::uint32_t> readVarint(std::span<const std::byte> in, std::size_t& offset);

} // namespace dh::varint
