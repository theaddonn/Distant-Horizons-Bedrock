// dh/build/DecorationOrder.h: the vanilla Bedrock decoration seed arithmetic and parity order (Run 13).
#include <doctest.h>

#include <cstdint>
#include <initializer_list>

#include "dh/build/DecorationOrder.h"

using namespace dh;

TEST_CASE("decoration seed multiplier: a/2*2+1 == (a + (a>>>31)) | 1 (compiled form)") {
    for (std::int64_t v : {0LL, 1LL, 2LL, -1LL, -2LL, -5LL, 7LL, 2147483647LL, -2147483648LL, 123456789LL, -987654321LL}) {
        const auto          a        = static_cast<std::int32_t>(v);
        const std::uint32_t ua       = static_cast<std::uint32_t>(a);
        const std::uint32_t compiled = (ua + (ua >> 31)) | 1u;
        CHECK(decorationSeedMultiplier(a) == compiled);
        CHECK((decorationSeedMultiplier(a) & 1u) == 1u);
    }
    CHECK(decorationSeedMultiplier(-5) == static_cast<std::uint32_t>(-3));
    CHECK(decorationSeedMultiplier(6) == 7u);
}

TEST_CASE("decoration chunk seed: 32-bit wrap and xor with the level seed") {
    CHECK(decorationChunkSeed(0x12345678u, 3u, 5u, 0, 0) == 0x12345678u);
    CHECK(decorationChunkSeed(0u, 3u, 5u, 2, 1) == 11u);
    CHECK(decorationChunkSeed(0u, 3u, 5u, -1, 0) == static_cast<std::uint32_t>(-3));
    // wraps instead of widening to 64 bit
    CHECK(decorationChunkSeed(0u, 0x80000001u, 1u, 2, 0) == 2u);
    CHECK(decorationChunkSeed(0xFFFFFFFFu, 1u, 1u, 1, 1) == (0xFFFFFFFFu ^ 2u));
}

TEST_CASE("decoration parity order satisfies the DecorationParity neighbour tables") {
    // neighbour index order: row-major 3x3 without the centre
    constexpr int dxs[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    constexpr int dzs[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    // required neighbour state per table: 4 = structure-post-processed, 6 = decorated (RE @0xe840350..68)
    constexpr int tables[4][8] = {
        {4, 4, 4, 4, 4, 4, 4, 4},
        {4, 4, 4, 6, 6, 4, 4, 4},
        {6, 6, 6, 4, 4, 6, 6, 6},
        {6, 6, 6, 6, 6, 6, 6, 6},
    };
    for (int z = -3; z <= 3; ++z)
        for (int x = -3; x <= 3; ++x) {
            const int cls = decorationParityClass(x, z);
            CHECK(cls == (x & 1) + 2 * (z & 1));
            for (int i = 0; i < 8; ++i) {
                const int ncls = decorationParityClass(x + dxs[i], z + dzs[i]);
                // a neighbour that must be decorated first is always of a lower class ...
                if (tables[cls][i] == 6) CHECK(ncls < cls);
                // ... and every lower-class neighbour is one the table waits for (no neighbour decorated "early")
                if (ncls < cls) CHECK(tables[cls][i] == 6);
            }
        }
}
