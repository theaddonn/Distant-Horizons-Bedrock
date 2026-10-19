// Tests for the Stage-A render backend's pure vertex conversion (dh/mesh/StageAVertex).
#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dh/mesh/LodQuadBuilder.h"
#include "dh/mesh/StageAVertex.h"

using namespace dh;

namespace {

void put16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>(v >> 8));
}

void putVertex(
    std::vector<std::byte>& b,
    int                     x,
    int                     y,
    int                     z,
    unsigned                meta,
    std::uint8_t            r,
    std::uint8_t            g,
    std::uint8_t            bl,
    std::uint8_t            a
) {
    put16(b, static_cast<std::uint16_t>(x));
    put16(b, static_cast<std::uint16_t>(y));
    put16(b, static_cast<std::uint16_t>(z));
    put16(b, static_cast<std::uint16_t>(meta));
    b.push_back(static_cast<std::byte>(r));
    b.push_back(static_cast<std::byte>(g));
    b.push_back(static_cast<std::byte>(bl));
    b.push_back(static_cast<std::byte>(a));
    b.push_back(std::byte{0}); // material
    b.push_back(std::byte{1}); // normal
    put16(b, 0);               // tile
}

// Unit square at y=10; (x,z) corners (2,3) (2,4) (3,4) (3,3); sky 15, colour r=200 g=100 b=50 a=255.
std::vector<std::byte> handQuad(const unsigned* microPerVertex = nullptr) {
    std::vector<std::byte> b;
    const int              xs[4] = {2, 2, 3, 3};
    const int              zs[4] = {3, 4, 4, 3};
    for (int i = 0; i < 4; ++i) {
        unsigned meta = 15u | (microPerVertex ? (microPerVertex[i] << 8) : 0u);
        putVertex(b, xs[i], 10, zs[i], meta, 200, 100, 50, 255);
    }
    return b;
}

} // namespace

TEST_CASE("stagea: lightFactor curve") {
    CHECK(lightFactor(15, 0, 1.0f) == doctest::Approx(1.0f));
    CHECK(lightFactor(0, 15, 0.0f) == doctest::Approx(1.0f)); // block light ignores sky brightness
    CHECK(lightFactor(15, 0, 0.0f) == doctest::Approx(lightFactor(0, 0, 1.0f)));
    CHECK(lightFactor(0, 0, 1.0f) == doctest::Approx(0.04f + 0.96f * std::pow(0.8f, 15.0f)));
    CHECK(lightFactor(8, 3, 1.0f) < lightFactor(9, 3, 1.0f));
    CHECK(lightFactor(40, 0, 1.0f) == doctest::Approx(1.0f)); // clamped
}

TEST_CASE("stagea: quad list conversion") {
    auto                      buf = handQuad();
    std::vector<StageAVertex> out;
    CHECK(convertDhBuffer(buf, 6, -64.0f, out, Primitive::QuadList) == 1);
    REQUIRE(out.size() == 4);
    CHECK(out[0].x == 2.0f);
    CHECK(out[0].y == -54.0f);
    CHECK(out[0].z == 3.0f);
    CHECK(out[2].x == 3.0f);
    CHECK(out[2].z == 4.0f);
    // sky 15 at full brightness -> factor 1 -> colour unchanged; r in the low byte.
    CHECK(out[0].rgba == 0xFF3264C8u);
    // u = direction index (handQuad writes normal 1 = up), v = packed light (sky 15, block 0)
    for (auto& vx : out) {
        CHECK(vx.u == 1.0f);
        CHECK(vx.v == doctest::Approx(240.0f / 255.0f));
    }
}

TEST_CASE("stagea: shader lighting packs light into v and leaves the colour unlit") {
    std::vector<std::byte> b;
    for (int i = 0; i < 4; ++i) putVertex(b, i, 0, 0, 3u | (9u << 4), 200, 100, 50, 255); // sky 3, block 9
    std::vector<StageAVertex> out;
    convertDhBuffer(b, 6, 0.0f, out, Primitive::QuadList);
    REQUIRE(out.size() == 4);
    for (auto& vx : out) {
        CHECK(vx.rgba == 0xFF3264C8u); // not multiplied by lightFactor
        const int packed = static_cast<int>(std::lround(vx.v * 255.0f));
        CHECK((packed >> 4) == 3); // sky in the high nibble
        CHECK((packed & 15) == 9); // block in the low nibble
    }
    ConvertParams p;
    p.lightmapSwapAxes = true;
    out.clear();
    convertDhBuffer(b, 6, 0.0f, out, Primitive::QuadList, p);
    const int sw = static_cast<int>(std::lround(out[0].v * 255.0f));
    CHECK((sw >> 4) == 9);
    CHECK((sw & 15) == 3);
}

TEST_CASE("stagea: cpuLighting keeps the old behaviour (colour scaled, v = 0)") {
    std::vector<std::byte> b;
    for (int i = 0; i < 4; ++i) putVertex(b, i, 0, 0, 0u, 200, 100, 50, 255); // sky 0, block 0
    ConvertParams p;
    p.cpuLighting = true;
    std::vector<StageAVertex> out;
    convertDhBuffer(b, 6, 0.0f, out, Primitive::QuadList, p);
    REQUIRE(out.size() == 4);
    CHECK(out[0].rgba != 0xFF3264C8u);
    CHECK((out[0].rgba & 0xFFu) < 200u);
    CHECK(out[0].v == 0.0f);
}

TEST_CASE("stagea: direction index travels in u") {
    for (int dir = 0; dir < 6; ++dir) {
        std::vector<std::byte> b;
        for (int i = 0; i < 4; ++i) {
            putVertex(b, i, 0, 0, 15u, 1, 2, 3, 255);
            b[b.size() - 3] = static_cast<std::byte>(dir); // normal byte (offset 13 of the 16-byte vertex)
        }
        std::vector<StageAVertex> out;
        convertDhBuffer(b, 6, 0.0f, out, Primitive::TriangleList);
        REQUIRE(out.size() == 6);
        for (auto& vx : out) CHECK(vx.u == static_cast<float>(dir));
    }
}

TEST_CASE("stagea: triangle list conversion, flip and double sided") {
    auto buf = handQuad();
    {
        std::vector<StageAVertex> out, quad;
        convertDhBuffer(buf, 6, 0.0f, out, Primitive::TriangleList);
        convertDhBuffer(buf, 6, 0.0f, quad, Primitive::QuadList);
        REQUIRE(out.size() == 6);
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            CHECK(out[static_cast<size_t>(i)].x == quad[static_cast<size_t>(idx[i])].x);
            CHECK(out[static_cast<size_t>(i)].z == quad[static_cast<size_t>(idx[i])].z);
        }
    }
    {
        ConvertParams p;
        p.flipWinding = true;
        std::vector<StageAVertex> out;
        convertDhBuffer(buf, 6, 0.0f, out, Primitive::TriangleList, p);
        REQUIRE(out.size() == 6);
        // flipped: (0,2,1) (0,3,2)
        CHECK(out[1].x == 3.0f);
        CHECK(out[1].z == 4.0f); // vertex 2
        CHECK(out[2].x == 2.0f);
        CHECK(out[2].z == 4.0f); // vertex 1
        CHECK(out[4].x == 3.0f);
        CHECK(out[4].z == 3.0f); // vertex 3
    }
    {
        ConvertParams p;
        p.doubleSided = true;
        std::vector<StageAVertex> out;
        convertDhBuffer(buf, 6, 0.0f, out, Primitive::TriangleList, p);
        CHECK(out.size() == 12);
        out.clear();
        convertDhBuffer(buf, 6, 0.0f, out, Primitive::QuadList, p);
        REQUIRE(out.size() == 8);
        CHECK(out[4].x == 3.0f); // reversed copy starts at vertex 3
        CHECK(out[4].z == 3.0f);
        CHECK(out[7].x == 2.0f); // and ends at vertex 0
        CHECK(out[7].z == 3.0f);
    }
}

TEST_CASE("stagea: micro offset signs") {
    // vertex 0: x +, y -, z +; vertex 1: x -, z -; vertices 2/3: none.
    const unsigned            micro[4] = {0b01u | (0b11u << 2) | (0b01u << 4), 0b11u | (0b11u << 4), 0, 0};
    auto                      buf      = handQuad(micro);
    std::vector<StageAVertex> out;
    convertDhBuffer(buf, 6, 0.0f, out, Primitive::QuadList);
    REQUIRE(out.size() == 4);
    CHECK(out[0].x == doctest::Approx(2.01f));
    CHECK(out[0].y == doctest::Approx(9.99f));
    CHECK(out[0].z == doctest::Approx(3.01f));
    CHECK(out[1].x == doctest::Approx(1.99f));
    CHECK(out[1].y == doctest::Approx(10.0f));
    CHECK(out[1].z == doctest::Approx(3.99f));
    CHECK(out[2].x == 3.0f);
    CHECK(out[3].z == 3.0f);
}

TEST_CASE("stagea: light scales rgb but not alpha") {
    std::vector<std::byte> b;
    for (int i = 0; i < 4; ++i) putVertex(b, i, 0, 0, 0u /*sky 0, block 0*/, 255, 255, 255, 128);
    std::vector<StageAVertex> out;
    ConvertParams             cp;
    cp.cpuLighting = true;
    convertDhBuffer(b, 6, 0.0f, out, Primitive::QuadList, cp);
    REQUIRE(out.size() == 4);
    const std::uint32_t a  = out[0].rgba >> 24;
    const std::uint32_t r  = out[0].rgba & 0xFF;
    const float         lf = lightFactor(0, 0, 1.0f);
    CHECK(a == 128u);
    CHECK(r == static_cast<std::uint32_t>(255.0f * lf + 0.5f));
}

TEST_CASE("stagea: buffer size handling") {
    std::vector<StageAVertex> out;
    std::vector<std::byte>    two = handQuad();
    auto                      one = handQuad();
    two.insert(two.end(), one.begin(), one.end());
    CHECK(convertDhBuffer(two, 6, 0.0f, out, Primitive::QuadList) == 2);
    CHECK(out.size() == 8);
    out.clear();
    CHECK(convertDhBuffer(two, 6, 0.0f, out, Primitive::TriangleList) == 2);
    CHECK(out.size() == 12);
    CHECK(verticesPerQuad(Primitive::TriangleList) == 6);
    CHECK(verticesPerQuad(Primitive::QuadList, true) == 8);

    out.clear();
    std::vector<std::byte> partial(two.begin(), two.begin() + 127); // 1 quad + 63 stray bytes
    CHECK(convertDhBuffer(partial, 6, 0.0f, out, Primitive::QuadList) == 1);
    CHECK(out.size() == 4);
    out.clear();
    CHECK(convertDhBuffer(std::span<const std::byte>{}, 6, 0.0f, out, Primitive::QuadList) == 0);
    CHECK(out.empty());
}

TEST_CASE("stagea: LodQuadBuilder output converts to the expected vertex count") {
    LodQuadBuilder qb;
    REQUIRE(qb.addQuadUp(0, 5, 0, 4, 0xFF336699u, 0, 0, 15, 0));
    qb.mergeQuads();
    MeshBuffers mb = qb.build();
    REQUIRE(mb.opaque.size() == 1);
    CHECK(mb.opaque[0].size() % kQuadBytes == 0);
    std::vector<StageAVertex> out;
    std::size_t               n = convertDhBuffer(mb.opaque[0], 6, 0.0f, out, Primitive::TriangleList);
    CHECK(n == 1);
    CHECK(out.size() == 6);
    for (auto& v : out) {
        CHECK(v.y == doctest::Approx(5.0f).epsilon(0.01));
        CHECK(v.x >= -0.011f);
        CHECK(v.x <= 4.011f);
    }
}

TEST_CASE("stagea: GPU face encoding stays inside [0, 1], non-zero and round-trips") {
    for (int f = 0; f <= 5; ++f) {
        const float u = encodeFaceU(static_cast<float>(f));
        CHECK(u > 0.06f); // the VS sentinel: below this uv0 is treated as missing
        CHECK(u <= 1.0f);
        CHECK(static_cast<int>(std::lround(u * 8.0f)) - 1 == f);
    }
}
