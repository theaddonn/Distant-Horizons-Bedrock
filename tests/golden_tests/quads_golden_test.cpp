// Golden vectors for LodQuadBuilder (tests/golden/quads.gv.json). Every out key is checked
// (opaqueQuads, transparentQuads, opaque[], transparent[]); SKIPPED_KEYS: none.
// The dumper's client level: dirt colour = colour of block 2 / biome 0, shades Down .5, Up 1, N/S .8, W/E .6.
#include <doctest.h>

#include <cstdint>
#include <string>

#include "dh/mesh/LodQuadBuilder.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

namespace {

std::string toHexStr(const std::vector<std::byte>& b) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    s.reserve(b.size() * 2);
    for (std::byte x : b) {
        s.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        s.push_back(d[std::to_integer<unsigned>(x) & 15]);
    }
    return s;
}

std::uint32_t dirtColor() {
    const std::uint32_t b = 2u * 0x9E3779B1u;
    const std::uint32_t m = 0u * 0x85EBCA6Bu;
    return 0xFF000000u | ((b ^ m) & 0xFFFFFFu);
}

} // namespace

TEST_CASE("golden quads: LodQuadBuilder") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "quads");
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx            c(gc, st);
        const auto&        in = gc.in;
        QuadBuilderOptions o;
        o.transparency      = in.at("doTransparency").get<bool>() ? Transparency::Complete : Transparency::Disabled;
        o.grassSide         = static_cast<GrassSideRendering>(in.at("grassSideRendering").get<int>());
        o.debugRenderMode   = in.at("debugRendering").get<int>() != 0;
        o.earthCurveEnabled = in.at("earthCurveRatio").get<int>() != 0;
        o.showOverlappingQuadErrors = in.at("showOverlappingQuadErrors").get<bool>();
        o.dirtColorArgb             = dirtColor();
        o.shadeByFace               = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};

        LodQuadBuilder b(o);
        for (const auto& call : in.at("calls")) {
            const std::string k = call[0].get<std::string>();
            auto              a = [&](std::size_t i) { return call[i].get<std::int64_t>(); };
            if (k == "adj") {
                b.addQuadAdj(
                    static_cast<Direction>(a(1)),
                    static_cast<std::int16_t>(a(2)),
                    static_cast<std::int16_t>(a(3)),
                    static_cast<std::int16_t>(a(4)),
                    static_cast<std::int16_t>(a(5)),
                    static_cast<std::int16_t>(a(6)),
                    static_cast<std::uint32_t>(a(7)),
                    static_cast<std::uint16_t>(a(8)),
                    static_cast<std::uint8_t>(a(9)),
                    static_cast<std::uint8_t>(a(10)),
                    static_cast<std::uint8_t>(a(11))
                );
            } else if (k == "up" || k == "down") {
                auto f = k == "up" ? &LodQuadBuilder::addQuadUp : &LodQuadBuilder::addQuadDown;
                (b.*f)(
                    static_cast<std::int16_t>(a(1)),
                    static_cast<std::int16_t>(a(2)),
                    static_cast<std::int16_t>(a(3)),
                    static_cast<std::int16_t>(a(4)),
                    static_cast<std::uint32_t>(a(5)),
                    static_cast<std::uint16_t>(a(6)),
                    static_cast<std::uint8_t>(a(7)),
                    static_cast<std::uint8_t>(a(8)),
                    static_cast<std::uint8_t>(a(9))
                );
            } else {
                FAIL("unknown call " << k);
            }
        }
        b.mergeQuads();
        c.eq("opaqueQuads", b.getCurrentOpaqueQuadsCount());
        c.eq("transparentQuads", b.getCurrentTransparentQuadsCount());

        const MeshBuffers m  = b.build();
        const auto&       eo = gc.out.at("opaque");
        const auto&       et = gc.out.at("transparent");
        INFO("case ", gc.name);
        CHECK_EQ(m.opaque.size(), eo.size());
        CHECK_EQ(m.transparent.size(), et.size());
        for (std::size_t i = 0; i < eo.size() && i < m.opaque.size(); ++i) {
            ++st.checked;
            INFO("case ", gc.name, " key opaque[", i, "]");
            CHECK_EQ(toHexStr(m.opaque[i]), eo[i].get<std::string>());
        }
        for (std::size_t i = 0; i < et.size() && i < m.transparent.size(); ++i) {
            ++st.checked;
            INFO("case ", gc.name, " key transparent[", i, "]");
            CHECK_EQ(toHexStr(m.transparent[i]), et[i].get<std::string>());
        }
    }
    gv::report("quads", st);
}
