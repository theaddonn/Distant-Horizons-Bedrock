// dh/build/FeaturesBatchPlan.h: which chunks the Bedrock Features mode generates, structure-processes, decorates
// and delivers per batch (Run 15: delivery gating, parity order).
#include <doctest.h>

#include <set>
#include <string>

#include "dh/build/FeaturesBatchPlan.h"

using namespace dh;

TEST_CASE("features plan: 4x4 batch sizes and vanilla dependencies") {
    for (int viewRing : {0, 1}) {
        const FeaturesBatchPlan p = makeFeaturesBatchPlan(-3, 5, 4, viewRing);
        CHECK(p.base.size() == static_cast<std::size_t>((8 + 2 * viewRing) * (8 + 2 * viewRing)));
        CHECK(p.structures.size() == 64u);
        CHECK(p.decorate.size() == 36u);
        CHECK(p.deliver.size() == 16u);
        CHECK(std::string(checkFeaturesBatchPlan(p, viewRing)).empty());
    }
}

TEST_CASE("features plan: decoration runs in ascending parity class, row-major inside a class") {
    const FeaturesBatchPlan p = makeFeaturesBatchPlan(0, 0, 2);
    int                     last = -1;
    ChunkXZ                 prev{};
    for (const auto& c : p.decorate) {
        const int cls = decorationParityClass(c.x, c.z);
        CHECK(cls >= last);
        if (cls == last) CHECK(prev < c);
        last = cls;
        prev = c;
    }
    // (even, odd) is the last class (Run 15 table re-trace), (odd, odd) the third
    CHECK(decorationParityClass(p.decorate.back().x, p.decorate.back().z) == 3);
    CHECK(decorationParityClass(1, 1) == 2);
    CHECK(decorationParityClass(0, 1) == 3);
    CHECK(decorationParityClass(-1, -1) == 2);
}

TEST_CASE("features plan: every delivered chunk is inside the batch and has its full 3x3 decorated") {
    for (int w : {1, 2, 4}) {
        const FeaturesBatchPlan p = makeFeaturesBatchPlan(7, -9, w);
        const std::set<ChunkXZ> deco(p.decorate.begin(), p.decorate.end());
        for (const auto& c : p.deliver) {
            CHECK(c.x >= 7);
            CHECK(c.x < 7 + w);
            CHECK(c.z >= -9);
            CHECK(c.z < -9 + w);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) CHECK(deco.count({c.x + dx, c.z + dz}) == 1);
        }
    }
}

TEST_CASE("features plan: the checker rejects broken plans") {
    FeaturesBatchPlan p = makeFeaturesBatchPlan(0, 0, 2);
    FeaturesBatchPlan q = p;
    q.deliver.push_back({-1, 0}); // a ring chunk
    CHECK(!std::string(checkFeaturesBatchPlan(q)).empty());
    q = p;
    std::reverse(q.decorate.begin(), q.decorate.end());
    CHECK(std::string(checkFeaturesBatchPlan(q)) == "lower-class neighbour decorated after the chunk");
    q = p;
    q.structures = squareRing(0, 0, 2, 1); // ring-1 cells would decorate next to unprocessed ring-2 cells
    CHECK(std::string(checkFeaturesBatchPlan(q)) == "decorated chunk with a neighbour not structure-processed");
    q = p;
    q.decorate = makeFeaturesBatchPlan(1, 1, 0).decorate; // empty: the ring (and the batch) not decorated
    CHECK(std::string(checkFeaturesBatchPlan(q)) == "delivered chunk with an undecorated chunk in its 3x3");
    CHECK(!std::string(checkFeaturesBatchPlan(p, 1)).empty()); // view ring 1 needs base ring 3
}

TEST_CASE("batchConstructionCells: Features = (w+4)^2 cells equal to the plan's base cells, Surface = w^2") {
    using namespace dh;
    for (std::int32_t w : {1, 2, 4, 8}) {
        const auto f = batchConstructionCells(true, -3, 5, w);
        const auto s = batchConstructionCells(false, -3, 5, w);
        CHECK(f.size() == static_cast<std::size_t>((w + 4) * (w + 4)));
        CHECK(s.size() == static_cast<std::size_t>(w * w));
        CHECK(f == makeFeaturesBatchPlan(-3, 5, w).base);
        CHECK(s == makeFeaturesBatchPlan(-3, 5, w).deliver);
    }
    CHECK(batchConstructionCells(true, 0, 0, 0).empty());
}
