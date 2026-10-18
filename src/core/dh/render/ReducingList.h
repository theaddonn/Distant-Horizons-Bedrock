#pragma once

#include <cstdint>
#include <vector>

namespace dh {

struct ColumnRenderView;

/// Port of Java RenderDataPointReducingList (core/util): reduces one render column to at most
/// `target` visible vertical slices by merging/erasing the smallest segments.
///
/// Every node is a (data, links) pair. `links` packs four 16-bit indices (lower, higher, smaller,
/// bigger; NULL = 0xFFFF). Nodes are kept in two doubly linked orders at once: lowest-to-highest
/// (by Y) and smallest-to-biggest (by height). Air gaps between visible segments are explicit nodes.
///
/// Not ported: the Java object pool (PhantomArrayList); buffers are plain vectors reused across
/// populate() calls. Not thread safe.
class ReducingList {
public:
    static constexpr int SPECIAL_CASES = 2;

    static constexpr int LOWER_SHIFT   = 0;
    static constexpr int HIGHER_SHIFT  = 16;
    static constexpr int SMALLER_SHIFT = 32;
    static constexpr int BIGGER_SHIFT  = 48;
    static constexpr int LINK_MASK     = 0xFFFF;
    static constexpr int NULL_LINK     = LINK_MASK; ///< Java NULL

    static constexpr std::uint64_t DEFAULT_DATA  = 0;
    static constexpr std::uint64_t DEFAULT_LINKS = ~std::uint64_t{0}; ///< Java -1L

    /// Java populate. Returns false where Java throws IllegalArgumentException (two visible
    /// segments overlap); the list is then empty-and-unusable (reduce/copyTo write a void column).
    bool populate(const ColumnRenderView& view);

    /// Merges/erases nodes until size() <= target.
    void reduce(int target);

    /// Writes the visible nodes highest-to-lowest into `view`, then pads with EMPTY_DATA up to
    /// view.size (a lone EMPTY_DATA when nothing is visible).
    void copyTo(ColumnRenderView& view) const;

    /// Java static reduceToOne: highest visible segment's properties with the lowest visible min Y.
    static std::uint64_t reduceToOne(const ColumnRenderView& view);

    /// Number of visible nodes (Java sizeWithoutAir).
    int size() const { return getSizeWithoutAir(); }
    int sizeWithAir() const { return getSizeWithAir(); }

    /// Java checkLinks as a predicate (true = consistent).
    bool checkLinks() const;

    static bool isAlphaVisible(int alpha) { return alpha >= 16; }
    static bool isDataVisible(std::uint64_t data);

    // Java public helpers (kept public for tests)
    void remove(int index);
    void sortBySizeAndReLink();
    void sortBySize(int size);
    void sortByPosition(int size);
    void resortSize(int smaller);

    int getSmallest() const { return smallest_ & LINK_MASK; }
    int getBiggest() const { return biggest_ & LINK_MASK; }
    int getLowest() const { return lowest_ & LINK_MASK; }
    int getHighest() const { return highest_ & LINK_MASK; }
    int getSizeWithAir() const { return sizeWithAir_ & LINK_MASK; }
    int getSizeWithoutAir() const { return sizeWithoutAir_ & LINK_MASK; }
    int getSortingIndex(int index) const { return sortingArray_[static_cast<std::size_t>(index)] & LINK_MASK; }
    int getLower(int i) const { return static_cast<int>(links_[idx(i)] >> LOWER_SHIFT) & LINK_MASK; }
    int getHigher(int i) const { return static_cast<int>(links_[idx(i)] >> HIGHER_SHIFT) & LINK_MASK; }
    int getSmaller(int i) const { return static_cast<int>(links_[idx(i)] >> SMALLER_SHIFT) & LINK_MASK; }
    int getBigger(int i) const { return static_cast<int>(links_[idx(i)] >> BIGGER_SHIFT) & LINK_MASK; }
    std::uint64_t getData(int i) const { return data_[idx(i)]; }
    int           getMinY(int i) const;
    int           getMaxY(int i) const;
    int           getSize(int i) const;
    int           getAlpha(int i) const;
    bool          isIndexVisible(int i) const { return isDataVisible(getData(i)); }

private:
    static std::size_t idx(int i) { return static_cast<std::size_t>(i); }

    void setSmallest(int v) { smallest_ = static_cast<std::uint16_t>(v); }
    void setBiggest(int v) { biggest_ = static_cast<std::uint16_t>(v); }
    void setLowest(int v) { lowest_ = static_cast<std::uint16_t>(v); }
    void setHighest(int v) { highest_ = static_cast<std::uint16_t>(v); }
    void setSizeWithAir(int v) { sizeWithAir_ = static_cast<std::uint16_t>(v); }
    void setSizeWithoutAir(int v) { sizeWithoutAir_ = static_cast<std::uint16_t>(v); }
    void setSortingIndex(int index, int to) { sortingArray_[idx(index)] = static_cast<std::uint16_t>(to); }
    void setLink(int index, int shift, int value);
    void setLower(int i, int v) { setLink(i, LOWER_SHIFT, v); }
    void setHigher(int i, int v) { setLink(i, HIGHER_SHIFT, v); }
    void setSmaller(int i, int v) { setLink(i, SMALLER_SHIFT, v); }
    void setBigger(int i, int v) { setLink(i, BIGGER_SHIFT, v); }
    void setData(int i, std::uint64_t d) { data_[idx(i)] = d; }
    void setMinY(int i, int minY);

    void clearEmpty();

    int  tryMergeStep1(int current, bool fastPath);
    int  lowerNode(int size) const;
    bool mergeVerySmallConnectedSegments(int target);
    bool mergeConnectedSegments(int target);
    bool removeLeastImportantSegments(int target);
    void forceBottomToMerge(int target);

    std::uint16_t lowest_ = 0, highest_ = 0, smallest_ = 0, biggest_ = 0;
    std::uint16_t sizeWithAir_ = 0, sizeWithoutAir_ = 0;

    std::vector<std::uint64_t> links_, data_;
    std::vector<std::uint16_t> sortingArray_;
};

} // namespace dh
