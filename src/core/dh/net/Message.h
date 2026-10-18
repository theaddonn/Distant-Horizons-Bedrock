#pragma once

// Network message base + codec helpers. Mirrors Java INetworkObject (netty ByteBuf, big-endian):
//   string   = u16 byte length + UTF-8 bytes
//   optional = bool(1 byte) + value when true
//   list     = i32 count + elements
//   enum     = 1 byte ordinal
// Decoders never trust a length: every count/size is bounded by the bytes that remain.

#include "dh/util/ByteIO.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dh::net {

using MsgId = std::uint16_t;

struct Message {
    virtual MsgId id() const                = 0;
    virtual void  encode(ByteWriter&) const = 0;
    /// Returns false on malformed/truncated input (message state then unspecified).
    virtual bool decode(ByteReader&) = 0;
    virtual ~Message()               = default;
};

namespace NetCodec {

inline constexpr std::size_t kMaxStringBytes = 0xFFFF;

inline void putBool(ByteWriter& w, bool v) { w.putU8(v ? 1 : 0); }
inline bool getBool(ByteReader& r, bool& out) {
    auto v = r.getU8();
    if (!v) return false;
    out = *v != 0; // netty readBoolean: any non-zero is true
    return true;
}

inline bool getI32(ByteReader& r, std::int32_t& out) {
    auto v = r.getI32BE();
    if (!v) return false;
    out = *v;
    return true;
}
inline bool getI64(ByteReader& r, std::int64_t& out) {
    auto v = r.getI64BE();
    if (!v) return false;
    out = *v;
    return true;
}

/// Strings longer than 65535 bytes are truncated (Java writeShort would wrap and corrupt the frame).
inline void putString(ByteWriter& w, std::string_view s) {
    const std::size_t n = s.size() > kMaxStringBytes ? kMaxStringBytes : s.size();
    w.putU16BE(static_cast<std::uint16_t>(n));
    w.putBytes({reinterpret_cast<const std::byte*>(s.data()), n});
}
inline bool getString(ByteReader& r, std::string& out) {
    auto len = r.getU16BE();
    if (!len) return false;
    auto bytes = r.getBytes(*len);
    if (!bytes) return false;
    out.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    return true;
}

/// `enc(ByteWriter&, const T&)`.
template <class T, class Enc>
void putOptional(ByteWriter& w, const std::optional<T>& v, Enc enc) {
    putBool(w, v.has_value());
    if (v) enc(w, *v);
}
/// `dec(ByteReader&, T&) -> bool`.
template <class T, class Dec>
bool getOptional(ByteReader& r, std::optional<T>& out, Dec dec) {
    bool present = false;
    if (!getBool(r, present)) return false;
    if (!present) {
        out.reset();
        return true;
    }
    T value{};
    if (!dec(r, value)) return false;
    out = std::move(value);
    return true;
}

template <class T, class Enc>
void putList(ByteWriter& w, const std::vector<T>& v, Enc enc) {
    w.putI32BE(static_cast<std::int32_t>(v.size()));
    for (const T& e : v) enc(w, e);
}
/// `minElementBytes` bounds the count against the remaining input.
template <class T, class Dec>
bool getList(ByteReader& r, std::vector<T>& out, std::size_t minElementBytes, Dec dec) {
    auto count = r.getI32BE();
    if (!count || *count < 0) return false;
    const std::size_t n = static_cast<std::size_t>(*count);
    if (minElementBytes == 0) minElementBytes = 1;
    if (n > r.remaining() / minElementBytes) return false;
    out.clear();
    out.reserve(n);
    for (std::size_t i = 0; i < n; i++) {
        T e{};
        if (!dec(r, e)) return false;
        out.push_back(std::move(e));
    }
    return true;
}

template <class E>
void putEnum(ByteWriter& w, E e) {
    w.putU8(static_cast<std::uint8_t>(e));
}
/// Fails when the ordinal is >= enumCount.
template <class E>
bool getEnum(ByteReader& r, E& out, std::uint8_t enumCount) {
    auto v = r.getU8();
    if (!v || *v >= enumCount) return false;
    out = static_cast<E>(*v);
    return true;
}

} // namespace NetCodec
} // namespace dh::net
