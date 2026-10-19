// Tests for the cloud geometry (client/render/CloudMesh) and the tiny PNG decoder (client/third_party/TinyPng.h).
// Both are Bedrock-free, so the client sources are compiled straight into the host test binary.
#include <doctest.h>

#include "../../src/client/render/CloudMesh.cpp"
#include "../../src/client/third_party/TinyPng.h"

#include "dh/mesh/StageAVertex.h"

#include <cstring>

using namespace dh::render;

namespace {

// 5x4 RGBA PNG (zlib deflate, filter chosen by Pillow): alpha 255 where (x + y) even or x == 2, else 1.
const std::uint8_t kPng[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49,
                             0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06,
                             0x00, 0x00, 0x00, 0x46, 0x33, 0xf5, 0x40, 0x00, 0x00, 0x00, 0x22, 0x49, 0x44,
                             0x41, 0x54, 0x78, 0x9c, 0x63, 0xfc, 0xff, 0xff, 0xff, 0x7f, 0x06, 0x06, 0x06,
                             0x26, 0x06, 0x06, 0x86, 0x7f, 0x30, 0x9a, 0x09, 0x49, 0x80, 0x01, 0x26, 0x81,
                             0xac, 0x82, 0x01, 0xaf, 0x4a, 0x00, 0x82, 0x8c, 0x0c, 0x04, 0x06, 0x31, 0x34,
                             0x8e, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::int16_t rd16s(std::byte const* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8));
}

} // namespace

TEST_CASE("tinypng decodes a Pillow-written RGBA PNG") {
    tinypng::Image img;
    REQUIRE(tinypng::decode(kPng, sizeof kPng, img));
    CHECK(img.width == 5);
    CHECK(img.height == 4);
    REQUIRE(img.rgba.size() == 5u * 4u * 4u);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x) {
            const std::uint8_t a = img.rgba[(static_cast<std::size_t>(y) * 5 + x) * 4 + 3];
            CHECK(img.rgba[(static_cast<std::size_t>(y) * 5 + x) * 4] == 255);
            CHECK(a == (((x + y) % 2 == 0 || x == 2) ? 255 : 1));
        }
    auto pat = cloud::patternFromRgba(img.rgba.data(), img.width, img.height);
    CHECK(pat.at(0, 0));
    CHECK_FALSE(pat.at(1, 0));
    CHECK(pat.at(5, 0));   // wraps
    CHECK(pat.at(-5, -4)); // negative wrap
    std::uint8_t bad[sizeof kPng];
    std::memcpy(bad, kPng, sizeof kPng);
    bad[0] = 0;
    CHECK_FALSE(tinypng::decode(bad, sizeof bad, img));
}

TEST_CASE("cloud mesh: a lone cell is a box of 6 quads") {
    cloud::Pattern pat;
    pat.w = pat.h = 4;
    pat.solid.assign(16, 0);
    pat.solid[0] = 1; // cell (0, 0) mod 4
    cloud::BuildParams bp;
    bp.cell       = 12;
    bp.centerX    = 0;
    bp.centerZ    = 0;
    bp.halfExtent = 24; // cells -2..1: exactly one solid cell, (0, 0)
    bp.yBottom    = 100;
    bp.thickness  = 4;
    auto b        = cloud::build(pat, bp);
    CHECK(b.quads == 6);
    REQUIRE(b.buffers.size() == 1);
    CHECK(b.buffers[0].size() == 6 * dh::kQuadBytes);
    int faces[6] = {};
    for (std::size_t q = 0; q < 6; ++q) {
        std::byte const* v = b.buffers[0].data() + q * dh::kQuadBytes;
        ++faces[static_cast<int>(v[13])];
        for (int i = 0; i < 4; ++i) {
            std::byte const* p = v + i * dh::kVertexBytes;
            CHECK(rd16s(p) >= 0);
            CHECK(rd16s(p) <= 12);
            CHECK(rd16s(p + 2) >= 100);
            CHECK(rd16s(p + 2) <= 104);
            CHECK(rd16s(p + 4) >= 0);
            CHECK(rd16s(p + 4) <= 12);
            CHECK(static_cast<int>(p[11]) == 204); // alpha
        }
    }
    for (int f = 0; f < 6; ++f) CHECK(faces[f] == 1);
}

TEST_CASE("cloud mesh: greedy merge and no artificial walls at the region edge") {
    cloud::Pattern pat;
    pat.w = pat.h = 4;
    pat.solid.assign(16, 1); // solid everywhere: one top + one bottom rectangle, no walls
    cloud::BuildParams bp;
    bp.halfExtent = 240;
    auto b        = cloud::build(pat, bp);
    CHECK(b.quads == 2);

    // flat layer: only the up/down pair per rectangle
    bp.fancy = false;
    cloud::Pattern one;
    one.w = one.h = 4;
    one.solid.assign(16, 0);
    one.solid[0]  = 1;
    bp.halfExtent = 24;
    CHECK(cloud::build(one, bp).quads == 2);
}

TEST_CASE("cloud mesh: procedural fallback pattern has plausible coverage and builds") {
    auto pat = cloud::proceduralPattern();
    CHECK(pat.w == 256);
    const double cov = static_cast<double>(pat.count()) / static_cast<double>(pat.solid.size());
    CHECK(cov > 0.05);
    CHECK(cov < 0.6);
    cloud::BuildParams bp;
    bp.halfExtent = 4096 + 384;
    auto b        = cloud::build(pat, bp);
    CHECK(b.quads > 100);
    for (auto const& buf : b.buffers) {
        CHECK(buf.size() % dh::kQuadBytes == 0);
        CHECK(buf.size() <= dh::kMaxBufferBytes);
    }
}
