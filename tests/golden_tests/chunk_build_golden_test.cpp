// Golden replay for LodDataBuilder.createFromChunk (tests/golden/chunk_build.gv.json).
//
// in.lit == true: lightChunk(chunk, [chunk], 15) runs first; otherwise the chunk is left unlit with both
// lighting-correct flags set (TestChunk's default light already matches the dumper's: sky 15 above the build
// height, 0 inside; block 0).
// Keys checked: pos, palette (serial pairs in id order), genStep, compMode (uniform over the chunk's 256 columns),
// isEmpty, applyToParent, offsetX, offsetZ, colTable/colRuns (all 256 chunk columns), otherNonEmpty.
// SKIPPED_KEYS: none. No `{"throws"}` values exist in this suite (nullopt from createFromChunk is a failure).
#include <doctest.h>

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "golden_check.h"
#include "golden_fixtures.h"

using namespace dh;
using namespace dh::test;
using gf::json;

TEST_CASE("golden chunk_build: LodDataBuilder.createFromChunk") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "chunk_build");
    gf::GoldenRegistry* reg = gf::GoldenRegistry::get();
    REQUIRE(reg != nullptr);
    gv::Stats st;

    for (const GoldenCase& gc : s.cases) {
        gv::Ctx c(gc, st);
        INFO("case ", gc.name);

        auto chunk = gf::fromGoldenChunk(gc.in.at("chunk"), *reg);
        if (gc.in.at("lit").get<bool>()) {
            IChunk* self[1] = {chunk.get()};
            lightChunk(*chunk, self, LightingOptions{});
        } else {
            chunk->setDhBlockLightingCorrect(true);
            chunk->setDhSkyLightingCorrect(true);
        }

        BuildOptions opts;
        opts.worldCompression = gc.in.at("worldCompression").get<int>() == 1 ? WorldCompressionMode::VisuallyEqual
                                                                             : WorldCompressionMode::MergeSameBlocks;
        auto ds               = createFromChunk(*chunk, opts);
        REQUIRE(ds.has_value());

        const int offX = gc.out.at("offsetX").get<int>(), offZ = gc.out.at("offsetZ").get<int>();
        c.eq("offsetX", chunkOffsetInSection(chunk->chunkX()));
        c.eq("offsetZ", chunkOffsetInSection(chunk->chunkZ()));
        c.eq("pos", ds->pos);
        c.eq("isEmpty", ds->isEmpty);
        ++st.checked;
        CHECK_EQ(ds->applyToParent.has_value() && *ds->applyToParent, gc.out.at("applyToParent").get<bool>());
        ++st.checked;
        CHECK_EQ(gf::diffPalette(ds->mapping, gc.out.at("palette")), std::string());
        ++st.checked; // genStep + compMode
        CHECK_EQ(
            gf::diffChunkUniform(*ds, offX, offZ, gc.out.at("genStep").get<int>(), gc.out.at("compMode").get<int>()),
            std::string()
        );
        ++st.checked; // colTable + colRuns
        CHECK_EQ(gf::diffChunkColumns(*ds, gc.out, offX, offZ), std::string());
        c.eq("otherNonEmpty", gf::countOtherNonEmpty(*ds, offX, offZ));
    }
    gv::report("chunk_bld", st);
}
