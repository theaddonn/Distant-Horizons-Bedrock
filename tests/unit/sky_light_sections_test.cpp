// "Black LOD section" regression (2026-10-19): a section produced through any path (generated, ingested chunk by
// chunk, reloaded from the store) must have sky light on every exposed top datapoint of its render data, so
// ColumnRenderSource::isDark() (-> HUD render.darkSections) stays false. Real block table, mixed terrain.
#include <doctest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/fulldata/Merge.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/IClock.h"
#include "dh/platform/TableClientLevel.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/FullToRender.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/storage/MemoryLodStore.h"
#include "dh/storage/SourceProvider.h"
#include "dh/thread/ManualExecutor.h"

using namespace dh;

namespace {

std::string slurpSky(const char* path) {
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

BlockTable& skyTable() {
    static BlockTable t = [] {
        auto r = BlockTable::load(slurpSky("../data/block_table.json"), slurpSky("../data/biome_table.json"));
        REQUIRE_MESSAGE(r.has_value(), "failed to load data/*.json");
        return std::move(r.value());
    }();
    return t;
}

/// Chunk (cx, cz) of the 4x4 section at (0,0): terrain kind chosen by index so one section mixes them all.
TestChunk makeChunk(int cx, int cz) {
    BlockTable& t = skyTable();
    TestChunk   c(cx, cz, -64, 320, t.air(), t.defaultBiome());
    c.setDhBlockLightingCorrect(true);
    auto* stone = t.resolveBlock("minecraft:stone");
    auto* sand  = t.resolveBlock("minecraft:sand");
    auto* water = t.resolveBlock("minecraft:water");
    auto* ice   = t.resolveBlock("minecraft:ice");
    auto* snow  = t.resolveBlock("minecraft:snow_layer[height=0]");
    auto* grass = t.resolveBlock("minecraft:grass_block");
    auto* plant = t.resolveBlock("minecraft:short_grass");
    REQUIRE((stone && sand && water && ice && snow && grass && plant));
    c.fillLayers(-64, 60, stone);
    switch ((cx + cz) % 5) {
    case 0: // snowy plain: grass block + snow layer
        c.fillLayers(60, 64, grass);
        c.fillLayer(64, snow);
        break;
    case 1: // frozen lake: sand, water, ice, snow on top
        c.fillLayer(60, sand);
        c.fillLayers(61, 63, water);
        c.fillLayer(63, ice);
        c.fillLayer(64, snow);
        break;
    case 2: // deep ocean (floor legitimately unlit, the water surface is not)
        c.fillLayers(10, 60, stone);
        c.fillLayers(0, 10, stone);
        c.fillLayers(-64, 10, stone);
        c.fillLayers(10, 62, water);
        break;
    case 3: // grass with transparent plants (opacity 0 but not air)
        c.fillLayers(60, 64, grass);
        c.fillLayer(64, plant);
        break;
    default: // bare stone plateau
        c.fillLayers(60, 70, stone);
        break;
    }
    return c;
}

struct SkyFx {
    ManualClock                     clock;
    ManualExecutor                  exec;
    MemoryLodStore                  store;
    std::unique_ptr<SourceProvider> provider;
    TableClientLevel                level{skyTable(), LevelInfo{}};
    const std::uint64_t             pos = sec::encode(6, 0, 0);

    SkyFx() { provider = std::make_unique<SourceProvider>(store, skyTable(), clock, exec, CodecOptions{}); }
    void reopen() { provider = std::make_unique<SourceProvider>(store, skyTable(), clock, exec, CodecOptions{}); }

    std::optional<FullDataSource> chunkSource(int cx, int cz) {
        TestChunk c = makeChunk(cx, cz);
        return createFromChunk(c, BuildOptions{});
    }

    /// Generated path (WorldGenerator::process): all chunks merged into one section source, one updater call.
    void generated() {
        auto result = std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));
        for (int cx = 0; cx < 4; ++cx)
            for (int cz = 0; cz < 4; ++cz) {
                auto built = chunkSource(cx, cz);
                REQUIRE(built.has_value());
                (void)updateFromDataSource(*result, *built, UpdateOptions{});
            }
        auto r = provider->updater().update(result);
        REQUIRE(r.has_value());
    }

    /// Ingested path (Level::runChunkUpdate): one updater call per chunk.
    void ingested() {
        for (int cx = 0; cx < 4; ++cx)
            for (int cz = 0; cz < 4; ++cz) {
                auto built = chunkSource(cx, cz);
                REQUIRE(built.has_value());
                auto r = provider->updater().update(std::make_shared<const FullDataSource>(std::move(*built)));
                REQUIRE(r.has_value());
            }
    }

    void check(const char* what) {
        auto src = provider->get(pos);
        REQUIRE(src);
        REQUIRE_FALSE(src->isEmpty);
        // full data: the top datapoint of every column is sky lit
        for (const auto& col : src->columns) {
            if (col.empty()) continue;
            CHECK_MESSAGE(fdp::skyLight(col.front()) == 15, what << ": top full datapoint not sky lit");
        }
        auto rs = transformFullDataToRenderSource(*src, level, skyTable(), TransformOptions{});
        REQUIRE(rs.has_value());
        CHECK_FALSE_MESSAGE(rs->isDark(), what << ": render data is dark");
        // render data: the top visible datapoint of every visible column is sky lit
        int visible = 0, lit = 0;
        for (int x = 0; x < ColumnRenderSource::WIDTH; ++x)
            for (int z = 0; z < ColumnRenderSource::WIDTH; ++z)
                for (int i = 0; i < rs->maxVerticalSlices; ++i) {
                    const auto d = rs->dataPoint(x, z, i);
                    if (d == rdp::EMPTY_DATA || rdp::hasZeroHeight(d)) continue;
                    ++visible;
                    if (rdp::skyLight(d) == 15) ++lit;
                    break;
                }
        CHECK(visible > 0);
        CHECK_MESSAGE(lit == visible, what << ": " << (visible - lit) << " of " << visible << " top datapoints unlit");
    }
};

} // namespace

TEST_CASE("sky light: generated section is sky lit after the updater bake") {
    SkyFx f;
    f.generated();
    f.check("generated");
}

TEST_CASE("sky light: chunk-by-chunk ingested section is sky lit") {
    SkyFx f;
    f.ingested();
    f.check("ingested");
}

TEST_CASE("sky light: section reloaded from the store keeps its sky light") {
    SkyFx f;
    f.generated();
    f.reopen(); // empty cache: get() decodes the stored record
    f.check("store round trip");
}

TEST_CASE("ColumnRenderSource::isDark: only a fully unlit top layer counts") {
    auto rs = ColumnRenderSource::createEmpty(sec::encode(6, 0, 0), 1, 0);
    CHECK_FALSE(rs.isDark()); // empty
    rs.markNotEmpty();
    CHECK_FALSE(rs.isDark()); // no visible column
    for (int x = 0; x < ColumnRenderSource::WIDTH; ++x)
        for (int z = 0; z < ColumnRenderSource::WIDTH; ++z)
            rs.data[static_cast<std::size_t>(ColumnRenderSource::dataIndex(x, z, 1, 0))] =
                rdp::create(70, 60, 0xFFFFFFFF, 0, 0, BlockMaterial::Unknown);
    CHECK(rs.isDark());
    rs.data[static_cast<std::size_t>(ColumnRenderSource::dataIndex(3, 4, 1, 0))] =
        rdp::create(70, 60, 0xFFFFFFFF, 15, 0, BlockMaterial::Unknown);
    CHECK_FALSE(rs.isDark());
}
