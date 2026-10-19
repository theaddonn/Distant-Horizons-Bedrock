#pragma once

// Minimal PNG decoder (non-interlaced, 8-bit grey / grey+alpha / RGB / RGBA / palette) with a small inflate.
// Written for this project (MIT licence): no external dependency, header only, no exceptions.
//
//   Copyright (c) 2026 the Distant Horizons Bedrock port authors
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software, to deal in it
//   without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute,
//   sublicense, and/or sell copies, subject to the above copyright notice being included in all copies. THE SOFTWARE
//   IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
//
// Output is always RGBA8. CRCs and the zlib Adler-32 are not verified (read-only game assets).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tinypng {

namespace detail {

struct BitReader {
    std::uint8_t const* p;
    std::size_t         n;
    std::size_t         pos    = 0;
    std::uint32_t       bitBuf = 0;
    int                 bitCnt = 0;
    bool                err    = false;

    std::uint32_t bits(int k) {
        while (bitCnt < k) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            bitBuf |= static_cast<std::uint32_t>(p[pos++]) << bitCnt;
            bitCnt += 8;
        }
        std::uint32_t v   = bitBuf & ((1u << k) - 1u);
        bitBuf          >>= k;
        bitCnt           -= k;
        return v;
    }
};

struct Huffman {
    std::array<std::uint16_t, 16>  count{};
    std::array<std::uint16_t, 288> symbol{};

    void build(std::uint8_t const* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        std::array<std::uint16_t, 16> offs{};
        for (int i = 1; i < 16; ++i) offs[i] = static_cast<std::uint16_t>(offs[i - 1] + count[i - 1]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(br.bits(1));
            if (br.err) return -1;
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index  += c;
            first  += c;
            first <<= 1;
            code  <<= 1;
        }
        return -1;
    }
};

inline bool inflateBlock(BitReader& br, std::vector<std::uint8_t>& out, Huffman const& lit, Huffman const& dist) {
    static constexpr std::uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::uint16_t lext[29]  = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::uint16_t dbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::uint16_t dext[30]  = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = lbase[sym] + static_cast<int>(br.bits(lext[sym]));
            int ds  = dist.decode(br);
            if (ds < 0 || ds >= 30) return false;
            std::size_t d = dbase[ds] + br.bits(dext[ds]);
            if (br.err || d > out.size()) return false;
            std::size_t from = out.size() - d;
            for (int i = 0; i < len; ++i) out.push_back(out[from + static_cast<std::size_t>(i)]);
        }
    }
}

/// zlib stream -> raw bytes.
inline bool inflateZlib(std::uint8_t const* data, std::size_t size, std::vector<std::uint8_t>& out) {
    if (size < 2) return false;
    BitReader br{data + 2, size - 2}; // skip the 2-byte zlib header
    bool      last = false;
    while (!last) {
        last         = br.bits(1) != 0;
        unsigned typ = br.bits(2);
        if (br.err) return false;
        if (typ == 0) {
            br.bitBuf = 0;
            br.bitCnt = 0;
            if (br.pos + 4 > br.n) return false;
            unsigned len  = br.p[br.pos] | (static_cast<unsigned>(br.p[br.pos + 1]) << 8);
            br.pos       += 4;
            if (br.pos + len > br.n) return false;
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
            br.pos += len;
        } else if (typ == 1) {
            std::uint8_t l[288];
            int          i = 0;
            for (; i < 144; ++i) l[i] = 8;
            for (; i < 256; ++i) l[i] = 9;
            for (; i < 280; ++i) l[i] = 7;
            for (; i < 288; ++i) l[i] = 8;
            Huffman lit, dist;
            lit.build(l, 288);
            std::uint8_t dl[30];
            std::memset(dl, 5, sizeof dl);
            dist.build(dl, 30);
            if (!inflateBlock(br, out, lit, dist)) return false;
        } else if (typ == 2) {
            unsigned nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
            if (br.err || nlen > 286 || ndist > 30) return false;
            static constexpr std::uint8_t order[19] =
                {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t lengths[320] = {};
            for (unsigned i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<std::uint8_t>(br.bits(3));
            Huffman lc;
            lc.build(lengths, 19);
            std::uint8_t all[320] = {};
            unsigned     idx      = 0;
            while (idx < nlen + ndist) {
                int sym = lc.decode(br);
                if (sym < 0) return false;
                if (sym < 16) {
                    all[idx++] = static_cast<std::uint8_t>(sym);
                } else {
                    std::uint8_t prev = 0;
                    unsigned     rep  = 0;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        prev = all[idx - 1];
                        rep  = 3 + br.bits(2);
                    } else if (sym == 17) rep = 3 + br.bits(3);
                    else rep = 11 + br.bits(7);
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) all[idx++] = prev;
                }
            }
            Huffman lit, dist;
            lit.build(all, static_cast<int>(nlen));
            dist.build(all + nlen, static_cast<int>(ndist));
            if (!inflateBlock(br, out, lit, dist)) return false;
        } else {
            return false;
        }
        if (br.err) return false;
    }
    return true;
}

inline std::uint32_t be32(std::uint8_t const* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

} // namespace detail

struct Image {
    int                       width = 0, height = 0;
    std::vector<std::uint8_t> rgba; ///< width * height * 4
};

/// Decodes a PNG file image. Returns false for anything unsupported (interlaced, 16-bit, sub-byte depth) or corrupt.
inline bool decode(std::uint8_t const* data, std::size_t size, Image& img) {
    static constexpr std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 33 || std::memcmp(data, sig, 8) != 0) return false;
    std::size_t               pos = 8;
    int                       w = 0, h = 0, depth = 0, ctype = -1, interlace = 0;
    std::vector<std::uint8_t> idat, palette, trns;
    while (pos + 12 <= size) {
        std::uint32_t len = detail::be32(data + pos);
        if (pos + 12 + static_cast<std::size_t>(len) > size) return false;
        char const*         type = reinterpret_cast<char const*>(data + pos + 4);
        std::uint8_t const* body = data + pos + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w         = static_cast<int>(detail::be32(body));
            h         = static_cast<int>(detail::be32(body + 4));
            depth     = body[8];
            ctype     = body[9];
            interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) palette.assign(body, body + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(body, body + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), body, body + len);
        else if (!std::memcmp(type, "IEND", 4)) break;
        pos += 12 + static_cast<std::size_t>(len);
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || depth != 8 || interlace != 0) return false;
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels) return false;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * channels + 1));
    if (!detail::inflateZlib(idat.data(), idat.size(), raw)) return false;
    const std::size_t stride = static_cast<std::size_t>(w) * channels;
    if (raw.size() < static_cast<std::size_t>(h) * (stride + 1)) return false;

    std::vector<std::uint8_t> pix(static_cast<std::size_t>(h) * stride);
    for (int y = 0; y < h; ++y) {
        std::uint8_t const* src  = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        std::uint8_t        ft   = src[0];
        std::uint8_t*       cur  = pix.data() + static_cast<std::size_t>(y) * stride;
        std::uint8_t const* prev = y ? cur - stride : nullptr;
        ++src;
        for (std::size_t i = 0; i < stride; ++i) {
            int a = i >= static_cast<std::size_t>(channels) ? cur[i - channels] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= static_cast<std::size_t>(channels)) ? prev[i - channels] : 0;
            int v = src[i];
            switch (ft) {
            case 0:
                break;
            case 1:
                v += a;
                break;
            case 2:
                v += b;
                break;
            case 3:
                v += (a + b) / 2;
                break;
            case 4: {
                int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default:
                return false;
            }
            cur[i] = static_cast<std::uint8_t>(v);
        }
    }

    img.width  = w;
    img.height = h;
    img.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        std::uint8_t const* s = pix.data() + i * channels;
        std::uint8_t*       d = img.rgba.data() + i * 4;
        switch (ctype) {
        case 0:
            d[0] = d[1] = d[2] = s[0];
            break;
        case 2:
            d[0] = s[0], d[1] = s[1], d[2] = s[2];
            break;
        case 3: {
            std::size_t k = static_cast<std::size_t>(s[0]) * 3;
            if (k + 2 < palette.size()) d[0] = palette[k], d[1] = palette[k + 1], d[2] = palette[k + 2];
            if (s[0] < trns.size()) d[3] = trns[s[0]];
            break;
        }
        case 4:
            d[0] = d[1] = d[2] = s[0];
            d[3]               = s[1];
            break;
        case 6:
            std::memcpy(d, s, 4);
            break;
        }
    }
    return true;
}

} // namespace tinypng
