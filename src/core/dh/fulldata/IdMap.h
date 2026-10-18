#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dh/platform/BlockInfo.h"

namespace dh {

/// Port of Java FullDataPointIdMap (in-memory part). Maps small integer ids to
/// (biome, block) pairs; id == index in the list. Not thread safe.
/// Identity is pointer equality of the interned BiomeInfo/BlockInfo.
/// The hash map is used for lookup only, so ids are fully deterministic.
class IdMap {
public:
    IdMap() = default;
    explicit IdMap(std::uint64_t pos) : sectionPos(pos) {}

    /// Returns the existing id for the pair or appends it and returns the new id.
    int addIfAbsent(const BiomeInfo* biome, const BlockInfo* block);

    /// nullptr when id is out of range.
    const BlockInfo* block(int id) const;
    const BiomeInfo* biome(int id) const;

    /// For every entry of `other` (in order) calls addIfAbsent; result[i] = new id of other's id i.
    std::vector<int> mergeAndRemap(const IdMap& other);

    /// Appends every entry of `other` WITHOUT deduplicating (ids stay aligned with the source).
    /// Like Java the lookup map then points at the LAST duplicate for a repeated pair.
    void addAllAllowingDups(const IdMap& other);

    std::size_t size() const { return mPairs.size(); }
    bool        empty() const { return mPairs.empty(); }
    /// -1 when empty (Java getMaxValidId)
    int maxValidId() const { return static_cast<int>(mPairs.size()) - 1; }

    void clear() { clear(sectionPos); }
    void clear(std::uint64_t newPos);

    /// Order-sensitive equality of the entry lists (ids included).
    bool sameEntries(const IdMap& other) const;

    std::uint64_t sectionPos = 0; ///< debugging only

private:
    struct Pair {
        const BiomeInfo* biome;
        const BlockInfo* block;
        bool             operator==(const Pair& o) const { return biome == o.biome && block == o.block; }
    };
    struct PairHash {
        std::size_t operator()(const Pair& p) const {
            const std::uint64_t a  = reinterpret_cast<std::uintptr_t>(p.biome);
            const std::uint64_t b  = reinterpret_cast<std::uintptr_t>(p.block);
            std::uint64_t       h  = (a * 0x9E3779B97F4A7C15ull) ^ ((b + 0x7F4A7C15ull) * 0xBF58476D1CE4E5B9ull);
            h                     ^= h >> 29;
            return static_cast<std::size_t>(h);
        }
    };

    std::vector<Pair>                       mPairs;
    std::unordered_map<Pair, int, PairHash> mLookup;
};

} // namespace dh
