// Section borders: once the neighbour's data is available through the provider's adjacent-strip
// path (LodRenderSection::renderSourceForPos), a flat, identical neighbour must cull every side
// face on the shared border; a missing neighbour renders the full wall (Java ColumnBox, adj empty).
#include <doctest.h>

#include <memory>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/fulldata/FullDataSource.h"
#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/testing/TestClientLevel.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/FullToRender.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/storage/SourceProvider.h"
#include "dh/thread/ManualExecutor.h"
#include "golden_loader.h"

using namespace dh;

namespace {

struct BorderFx {
    MapBlockRegistry                reg{false};
    ManualClock                     clock;
    ManualExecutor                  fileExec;
    MemoryLodStore                  store;
    TestClientLevel                 level;
    TransformOptions                topt;
    MeshOptions                     mopt;
    const BlockInfo*                stone = nullptr;
    std::unique_ptr<SourceProvider> provider;

    BorderFx() {
        BlockInfo s;
        s.serial               = "minecraft:stone";
        s.opacity              = 16;
        stone                  = reg.addBlock(std::move(s));
        level.minY             = -64;
        topt.enableCaveCulling = false;
        mopt.enableCaveCulling = false;
        provider = std::make_unique<SourceProvider>(store, reg, clock, fileExec, CodecOptions{}, ProviderOptions{});
    }

    void put(int x, int z, int sky) {
        auto s  = std::make_shared<FullDataSource>(FullDataSource::createEmpty(sec::encode(6, x, z)));
        int  id = s->mapping.addIfAbsent(reg.defaultBiome(), stone);
        for (int cx = 0; cx < FullDataSource::WIDTH; ++cx)
            for (int cz = 0; cz < FullDataSource::WIDTH; ++cz)
                s->setSingleColumn({fdp::encode(id, 130, 0, 0, static_cast<std::uint8_t>(sky))}, cx, cz, 5, 0);
        s->isEmpty = false;
        REQUIRE(provider->updater().update(s));
    }

    /// Mirrors LodRenderSection::getAndBuildRenderData for the section at (x, z): fetch + transform
    /// the centre and each present neighbour's strip, then mesh.
    int sideQuads(int x, int z, Direction side) {
        const sec::Pos pos  = sec::encode(6, x, z);
        auto           self = transformFullDataToRenderSource(*provider->get(pos), level, reg, topt);
        REQUIRE(self.has_value());
        std::optional<ColumnRenderSource> adjSrc[4];
        AdjacentSources                   adj;
        for (Direction d : {Direction::North, Direction::South, Direction::East, Direction::West}) {
            const int ci = dir::compassIndex(d);
            auto      r  = provider->getAdj(sec::adjacent(pos, d), dir::opposite(d));
            REQUIRE(r);
            adjSrc[ci]                                   = transformFullDataToRenderSource(**r, level, reg, topt);
            adj.bySide[static_cast<std::size_t>(ci)]     = adjSrc[ci].has_value() ? &*adjSrc[ci] : nullptr;
            adj.sameDetail[static_cast<std::size_t>(ci)] = true;
        }
        LodQuadBuilder b(mopt.quad);
        REQUIRE(makeLodRenderData(b, *self, level, adj, mopt));
        return static_cast<int>(b.opaqueQuads(side).size());
    }
};

} // namespace

// Java ColumnBox.java:374-393 deliberately keeps ONE overdraw face on a LOD border whenever the adjacent
// point is lit (the update pipeline's lighting gives every surface column sky light 15), so a present identical
// neighbour does NOT remove the border wall; such faces lie on the border plane inside solid terrain and are
// hidden by the depth test. What must hold: exactly one merged quad on the border plane, none interior.
TEST_CASE("section border: present lit neighbour keeps exactly one border overdraw face, on the border plane only") {
    BorderFx f;
    f.put(0, 0, 15);
    CHECK(f.sideQuads(0, 0, Direction::East) == 1); // neighbour absent: full wall
    f.put(1, 0, 15);
    CHECK(f.sideQuads(0, 0, Direction::East) == 1); // after the neighbour arrives: border overdraw, merged
    CHECK(f.sideQuads(1, 0, Direction::West) == 1);
    CHECK(f.sideQuads(0, 0, Direction::South) == 1);
}
