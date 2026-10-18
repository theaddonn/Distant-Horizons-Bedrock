#include "client/render/TestFeed.h"

#include "bedrock_common/FileLogger.h"
#include "client/render/LodRenderer.h"

#include "dh/mesh/LodQuadBuilder.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/Enums.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace dh::render::testfeed {

namespace {

std::vector<std::uint64_t> gPositions;

constexpr std::int8_t kDetail = 6; // 64-block sections

std::int16_t localY(double worldY, float minY) { return static_cast<std::int16_t>(std::lround(worldY - minY)); }

// Thin double-sided sheet column (up + down quad) so it is visible whichever way the winding is culled.
void sheet(LodQuadBuilder& qb, int x, int y, int z, int w, std::uint32_t argb) {
    qb.addQuadUp(
        static_cast<std::int16_t>(x),
        static_cast<std::int16_t>(y),
        static_cast<std::int16_t>(z),
        static_cast<std::int16_t>(w),
        argb,
        0,
        0,
        15,
        0
    );
    qb.addQuadDown(
        static_cast<std::int16_t>(x),
        static_cast<std::int16_t>(y),
        static_cast<std::int16_t>(z),
        static_cast<std::int16_t>(w),
        argb,
        0,
        0,
        15,
        0
    );
}

} // namespace

bool active() { return !gPositions.empty(); }
void reset() { gPositions.clear(); }

void toggle(LodRenderer& renderer, glm::dvec3 const& playerPos) {
    auto& sink = renderer.sink();
    if (active()) {
        for (auto p : gPositions) sink.remove(p);
        DH_LOG_I("render: test feed removed (%zu sections)", gPositions.size());
        gPositions.clear();
        return;
    }

    const float        minY = renderer.config().minYBlocks;
    const double       base = std::floor(playerPos.y - 1.62) + 4.0; // sheets float 4 blocks above the player's feet
    const int          sx   = static_cast<int>(std::floor(playerPos.x / 64.0));
    const int          sz   = static_cast<int>(std::floor(playerPos.z / 64.0));
    const std::int16_t yTop = localY(base, minY);

    QuadBuilderOptions opt;
    opt.transparency = Transparency::Complete;

    std::size_t sections = 0, quads = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            LodQuadBuilder qb(opt);
            if (dx == 0 && dz == 0) {
                // 64x64 checkerboard of 1-block columns, alternating height and colour.
                for (int z = 0; z < 64; ++z)
                    for (int x = 0; x < 64; ++x) {
                        bool          odd = ((x + z) & 1) != 0;
                        std::uint32_t col = odd ? 0xFFF2C14Eu : 0xFF2E86ABu;
                        sheet(qb, x, yTop + (odd ? 1 : 0), z, 1, col);
                    }
                // one tall pillar (4x4, 40 blocks) with four sides and a cap
                const int           px = 8, pz = 8, pw = 4, ph = 40;
                const std::uint32_t red = 0xFFD62828u;
                qb.addQuadAdj(
                    Direction::North,
                    static_cast<std::int16_t>(px),
                    yTop,
                    static_cast<std::int16_t>(pz),
                    pw,
                    ph,
                    red,
                    0,
                    0,
                    15,
                    0
                );
                qb.addQuadAdj(
                    Direction::South,
                    static_cast<std::int16_t>(px),
                    yTop,
                    static_cast<std::int16_t>(pz + pw),
                    pw,
                    ph,
                    red,
                    0,
                    0,
                    15,
                    0
                );
                qb.addQuadAdj(
                    Direction::West,
                    static_cast<std::int16_t>(px),
                    yTop,
                    static_cast<std::int16_t>(pz),
                    pw,
                    ph,
                    red,
                    0,
                    0,
                    15,
                    0
                );
                qb.addQuadAdj(
                    Direction::East,
                    static_cast<std::int16_t>(px + pw),
                    yTop,
                    static_cast<std::int16_t>(pz),
                    pw,
                    ph,
                    red,
                    0,
                    0,
                    15,
                    0
                );
                qb.addQuadUp(
                    static_cast<std::int16_t>(px),
                    static_cast<std::int16_t>(yTop + ph),
                    static_cast<std::int16_t>(pz),
                    pw,
                    red,
                    0,
                    0,
                    15,
                    0
                );
            } else {
                // neighbours: one flat 64x64 sheet each, distinct tint (merged big quad)
                static constexpr std::uint32_t tints[8] = {
                    0xFFB5E48Cu,
                    0xFF99D98Cu,
                    0xFF76C893u,
                    0xFF52B69Au,
                    0xFF34A0A4u,
                    0xFF168AADu,
                    0xFF1A759Fu,
                    0xFF1E6091u
                };
                int idx = (dz + 1) * 3 + (dx + 1);
                if (idx > 4) --idx;
                sheet(qb, 0, yTop, 0, 64, tints[idx]);
                if (dx == 1 && dz == 0) // translucent blue sheet 6 blocks higher: exercises the transparent layer
                    qb.addQuadUp(0, static_cast<std::int16_t>(yTop + 6), 0, 64, 0x802E86FFu, 0, 0, 15, 0);
            }
            qb.mergeQuads();
            const int n   = qb.getCurrentQuadsCount();
            auto      pos = sec::encode(kDetail, sx + dx, sz + dz);
            sink.upload(pos, qb.build(), nullptr);
            gPositions.push_back(pos);
            ++sections;
            quads += static_cast<std::size_t>(n);
        }
    }
    DH_LOG_I(
        "render: test feed uploaded %zu sections, %zu quads (centre section %d,%d min corner block (%d,%d), sheet "
        "y=%.0f)",
        sections,
        quads,
        sx,
        sz,
        sec::minCornerBlockX(sec::encode(kDetail, sx, sz)),
        sec::minCornerBlockZ(sec::encode(kDetail, sx, sz)),
        base
    );
}

} // namespace dh::render::testfeed
