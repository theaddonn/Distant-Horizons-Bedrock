#pragma once

// Bounds-checked byte writer/reader. No exceptions: reads return std::optional.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace dh {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) : mOut(&out) {}

    void putU8(std::uint8_t v) { mOut->push_back(static_cast<std::byte>(v)); }
    void putI8(std::int8_t v) { putU8(static_cast<std::uint8_t>(v)); }

    void putU16LE(std::uint16_t v) { putLE(v, 2); }
    void putU32LE(std::uint32_t v) { putLE(v, 4); }
    void putU64LE(std::uint64_t v) { putLE(v, 8); }
    void putI16LE(std::int16_t v) { putLE(static_cast<std::uint16_t>(v), 2); }
    void putI32LE(std::int32_t v) { putLE(static_cast<std::uint32_t>(v), 4); }
    void putI64LE(std::int64_t v) { putLE(static_cast<std::uint64_t>(v), 8); }
    void putF32LE(float v) { putU32LE(std::bit_cast<std::uint32_t>(v)); }
    void putF64LE(double v) { putU64LE(std::bit_cast<std::uint64_t>(v)); }

    /// Big-endian = Java DataOutputStream / netty ByteBuf default.
    void putU16BE(std::uint16_t v) { putBE(v, 2); }
    void putU32BE(std::uint32_t v) { putBE(v, 4); }
    void putU64BE(std::uint64_t v) { putBE(v, 8); }
    void putI16BE(std::int16_t v) { putBE(static_cast<std::uint16_t>(v), 2); }
    void putI32BE(std::int32_t v) { putBE(static_cast<std::uint32_t>(v), 4); }
    void putI64BE(std::int64_t v) { putBE(static_cast<std::uint64_t>(v), 8); }
    void putF32BE(float v) { putU32BE(std::bit_cast<std::uint32_t>(v)); }
    void putF64BE(double v) { putU64BE(std::bit_cast<std::uint64_t>(v)); }

    void putBytes(std::span<const std::byte> bytes) { mOut->insert(mOut->end(), bytes.begin(), bytes.end()); }

    std::size_t size() const { return mOut->size(); }

private:
    void putLE(std::uint64_t v, int n) {
        for (int i = 0; i < n; i++) putU8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void putBE(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) putU8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }

    std::vector<std::byte>* mOut;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> in) : mIn(in) {}

    std::size_t position() const { return mPos; }
    std::size_t remaining() const { return mIn.size() - mPos; }
    bool        atEnd() const { return mPos == mIn.size(); }
    /// Moves the cursor; returns false (cursor unchanged) when pos > size.
    bool seek(std::size_t pos) {
        if (pos > mIn.size()) return false;
        mPos = pos;
        return true;
    }
    bool skip(std::size_t n) {
        if (n > remaining()) return false;
        mPos += n;
        return true;
    }

    std::optional<std::uint8_t> getU8() { return readLE<std::uint8_t>(1); }
    std::optional<std::int8_t>  getI8() { return cast<std::int8_t>(getU8()); }

    std::optional<std::uint16_t> getU16LE() { return readLE<std::uint16_t>(2); }
    std::optional<std::uint32_t> getU32LE() { return readLE<std::uint32_t>(4); }
    std::optional<std::uint64_t> getU64LE() { return readLE<std::uint64_t>(8); }
    std::optional<std::int16_t>  getI16LE() { return cast<std::int16_t>(getU16LE()); }
    std::optional<std::int32_t>  getI32LE() { return cast<std::int32_t>(getU32LE()); }
    std::optional<std::int64_t>  getI64LE() { return cast<std::int64_t>(getU64LE()); }
    std::optional<float>         getF32LE() { return bits<float>(getU32LE()); }
    std::optional<double>        getF64LE() { return bits<double>(getU64LE()); }

    std::optional<std::uint16_t> getU16BE() { return readBE<std::uint16_t>(2); }
    std::optional<std::uint32_t> getU32BE() { return readBE<std::uint32_t>(4); }
    std::optional<std::uint64_t> getU64BE() { return readBE<std::uint64_t>(8); }
    std::optional<std::int16_t>  getI16BE() { return cast<std::int16_t>(getU16BE()); }
    std::optional<std::int32_t>  getI32BE() { return cast<std::int32_t>(getU32BE()); }
    std::optional<std::int64_t>  getI64BE() { return cast<std::int64_t>(getU64BE()); }
    std::optional<float>         getF32BE() { return bits<float>(getU32BE()); }
    std::optional<double>        getF64BE() { return bits<double>(getU64BE()); }

    /// Returns a view of the next n bytes and advances, or nullopt if fewer remain.
    std::optional<std::span<const std::byte>> getBytes(std::size_t n) {
        if (n > remaining()) return std::nullopt;
        auto s  = mIn.subspan(mPos, n);
        mPos   += n;
        return s;
    }

private:
    template <class T, class U>
    static std::optional<T> cast(std::optional<U> v) {
        if (!v) return std::nullopt;
        return static_cast<T>(*v);
    }
    template <class F, class U>
    static std::optional<F> bits(std::optional<U> v) {
        if (!v) return std::nullopt;
        return std::bit_cast<F>(*v);
    }
    template <class T>
    std::optional<T> readLE(std::size_t n) {
        if (n > remaining()) return std::nullopt;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; i++) v |= static_cast<std::uint64_t>(mIn[mPos + i]) << (8 * i);
        mPos += n;
        return static_cast<T>(v);
    }
    template <class T>
    std::optional<T> readBE(std::size_t n) {
        if (n > remaining()) return std::nullopt;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; i++) v = (v << 8) | static_cast<std::uint64_t>(mIn[mPos + i]);
        mPos += n;
        return static_cast<T>(v);
    }

    std::span<const std::byte> mIn;
    std::size_t                mPos = 0;
};

} // namespace dh
