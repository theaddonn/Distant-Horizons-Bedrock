#include "dh/build/ChunkCompare.h"

#include <algorithm>
#include <cstdio>

namespace dh {

namespace {

struct Top {
    std::int32_t     y     = 0;
    const BlockInfo* block = nullptr;
};

Top topOf(const IChunk& c, int x, int z) {
    for (std::int32_t y = c.exclusiveMaxBuildHeight() - 1; y >= c.inclusiveMinBuildHeight(); --y) {
        const BlockInfo* b = c.blockAt(x, y, z);
        if (!b->isAir) return {y, b};
    }
    return {c.inclusiveMinBuildHeight() - 1, nullptr};
}

bool isTree(const BlockInfo* b) {
    return b != nullptr && (b->material == BlockMaterial::Leaves || b->material == BlockMaterial::Wood);
}
bool isSnow(const BlockInfo* b) { return b != nullptr && b->material == BlockMaterial::Snow; }
bool isIce(const BlockInfo* b) { return b != nullptr && b->isIce; }

} // namespace

double ChunkDiff::treeIoU() const {
    const int uni = treeReal + treeGen - treeBoth;
    return uni == 0 ? 1.0 : static_cast<double>(treeBoth) / uni;
}

void ChunkDiff::add(const ChunkDiff& o) {
    columns += o.columns;
    sameTopY += o.sameTopY;
    sameTopBlock += o.sameTopBlock;
    treeReal += o.treeReal;
    treeGen += o.treeGen;
    treeBoth += o.treeBoth;
    snowReal += o.snowReal;
    snowGen += o.snowGen;
    iceReal += o.iceReal;
    iceGen += o.iceGen;
    diffBlocks += o.diffBlocks;
}

std::string ChunkDiff::summary() const {
    char buf[320];
    std::snprintf(
        buf,
        sizeof buf,
        "columns %d: topY same %d, top block same %d, tree columns real %d gen %d both %d (IoU %.2f), "
        "snow top real %d gen %d, ice top real %d gen %d, differing blocks %lld",
        columns,
        sameTopY,
        sameTopBlock,
        treeReal,
        treeGen,
        treeBoth,
        treeIoU(),
        snowReal,
        snowGen,
        iceReal,
        iceGen,
        static_cast<long long>(diffBlocks)
    );
    return buf;
}

ChunkDiff compareChunks(const IChunk& real, const IChunk& gen) {
    ChunkDiff d;
    const std::int32_t lo = std::max(real.inclusiveMinBuildHeight(), gen.inclusiveMinBuildHeight());
    const std::int32_t hi = std::min(real.exclusiveMaxBuildHeight(), gen.exclusiveMaxBuildHeight());
    for (int x = 0; x < IChunk::kWidth; ++x) {
        for (int z = 0; z < IChunk::kWidth; ++z) {
            ++d.columns;
            const Top r = topOf(real, x, z);
            const Top g = topOf(gen, x, z);
            if (r.y == g.y) {
                ++d.sameTopY;
                if (r.block == g.block) ++d.sameTopBlock;
            }
            const bool tr = isTree(r.block), tg = isTree(g.block);
            d.treeReal += tr;
            d.treeGen += tg;
            d.treeBoth += tr && tg;
            d.snowReal += isSnow(r.block);
            d.snowGen += isSnow(g.block);
            d.iceReal += isIce(r.block);
            d.iceGen += isIce(g.block);
            for (std::int32_t y = lo; y < hi; ++y)
                if (real.blockAt(x, y, z) != gen.blockAt(x, y, z)) ++d.diffBlocks;
        }
    }
    return d;
}

} // namespace dh
