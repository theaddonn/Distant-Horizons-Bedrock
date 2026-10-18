#include "dh/fulldata/IdMap.h"

namespace dh {

int IdMap::addIfAbsent(const BiomeInfo* biome, const BlockInfo* block) {
    const Pair pair{biome, block};
    auto it = mLookup.find(pair);
    if (it != mLookup.end()) return it->second;
    const int id = static_cast<int>(mPairs.size());
    mPairs.push_back(pair);
    mLookup.emplace(pair, id);
    return id;
}

const BlockInfo* IdMap::block(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= mPairs.size()) return nullptr;
    return mPairs[static_cast<std::size_t>(id)].block;
}

const BiomeInfo* IdMap::biome(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= mPairs.size()) return nullptr;
    return mPairs[static_cast<std::size_t>(id)].biome;
}

std::vector<int> IdMap::mergeAndRemap(const IdMap& other) {
    // copy first so merging a map into itself is well defined
    const std::vector<Pair> src = other.mPairs;
    std::vector<int> remap;
    remap.reserve(src.size());
    for (const Pair& p : src) remap.push_back(addIfAbsent(p.biome, p.block));
    return remap;
}

void IdMap::addAllAllowingDups(const IdMap& other) {
    const std::vector<Pair> src = other.mPairs;
    for (const Pair& p : src) {
        const int id = static_cast<int>(mPairs.size());
        mPairs.push_back(p);
        mLookup[p] = id; // overwrite: Java idMap.put
    }
}

void IdMap::clear(std::uint64_t newPos) {
    sectionPos = newPos;
    mPairs.clear();
    mLookup.clear();
}

bool IdMap::sameEntries(const IdMap& other) const { return mPairs == other.mPairs; }

} // namespace dh
