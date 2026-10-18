#include "dh/mesh/StageAVertex.h"

#include <algorithm>
#include <cmath>

#include "dh/platform/IVertexSink.h"

namespace dh {

float lightFactor(int sky, int block, float skyBrightness) {
    const float     s      = static_cast<float>(std::clamp(sky, 0, 15)) * std::clamp(skyBrightness, 0.0f, 1.0f);
    const float     b      = static_cast<float>(std::clamp(block, 0, 15));
    const float     l      = std::max(s, b);
    constexpr float kFloor = 0.04f;
    return kFloor + (1.0f - kFloor) * std::pow(0.8f, 15.0f - l);
}

namespace {

std::uint16_t rd16(const std::byte* p) {
    std::uint8_t a = static_cast<std::uint8_t>(p[0]), b = static_cast<std::uint8_t>(p[1]);
    return static_cast<std::uint16_t>(a | (b << 8));
}

// 2-bit micro field written by LodQuadBuilder::putVertex: bit0 = non-zero, bit1 = negative.
float microSign(unsigned bits) {
    if ((bits & 1u) == 0) return 0.0f;
    return (bits & 2u) ? -1.0f : 1.0f;
}

std::uint8_t scale(std::uint8_t c, float f) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<float>(c) * f + 0.5f, 0.0f, 255.0f));
}

} // namespace

std::size_t convertDhBuffer(
    std::span<const std::byte> dhQuads,
    int                        sectionDetail,
    float                      yOffsetBlocks,
    std::vector<StageAVertex>& out,
    Primitive                  mode,
    const ConvertParams&       params
) {
    (void)sectionDetail;
    const std::size_t quads = dhQuads.size() / kQuadBytes;
    out.reserve(out.size() + quads * verticesPerQuad(mode, params.doubleSided));

    static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    static constexpr float kV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    for (std::size_t q = 0; q < quads; ++q) {
        StageAVertex v[4];
        for (int i = 0; i < 4; ++i) {
            const std::byte* p     = dhQuads.data() + q * kQuadBytes + static_cast<std::size_t>(i) * kVertexBytes;
            const float      x     = static_cast<float>(static_cast<std::int16_t>(rd16(p + 0)));
            const float      y     = static_cast<float>(static_cast<std::int16_t>(rd16(p + 2)));
            const float      z     = static_cast<float>(static_cast<std::int16_t>(rd16(p + 4)));
            const unsigned   meta  = rd16(p + 6);
            const int        sky   = static_cast<int>(meta & 0xF);
            const int        block = static_cast<int>((meta >> 4) & 0xF);
            const unsigned   micro = (meta >> 8) & 0xFF;
            const float      light = lightFactor(sky, block, params.skyBrightness);

            v[i].x = x + microSign(micro & 3u) * params.microOffset;
            v[i].y = y + yOffsetBlocks + microSign((micro >> 2) & 3u) * params.microOffset;
            v[i].z = z + microSign((micro >> 4) & 3u) * params.microOffset;

            const std::uint8_t r = scale(static_cast<std::uint8_t>(p[8]), light);
            const std::uint8_t g = scale(static_cast<std::uint8_t>(p[9]), light);
            const std::uint8_t b = scale(static_cast<std::uint8_t>(p[10]), light);
            const std::uint8_t a = static_cast<std::uint8_t>(p[11]);
            v[i].rgba            = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8)
                      | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
            v[i].u = kU[i];
            v[i].v = kV[i];
        }

        auto emit = [&](bool reversed) {
            if (mode == Primitive::QuadList) {
                static constexpr int fwd[4] = {0, 1, 2, 3}, rev[4] = {3, 2, 1, 0};
                for (int k : (reversed ? rev : fwd)) out.push_back(v[k]);
            } else {
                static constexpr int fwd[6] = {0, 1, 2, 0, 2, 3}, rev[6] = {0, 2, 1, 0, 3, 2};
                for (int k : (reversed ? rev : fwd)) out.push_back(v[k]);
            }
        };
        emit(params.flipWinding);
        if (params.doubleSided) emit(!params.flipWinding);
    }
    return quads;
}

} // namespace dh
