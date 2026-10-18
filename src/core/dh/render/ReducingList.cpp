#include "dh/render/ReducingList.h"

#include <algorithm>
#include <cassert>

#include "dh/render/ColumnRenderSource.h"
#include "dh/render/RenderDataPoint.h"

namespace dh {

namespace {

constexpr int kNull = ReducingList::NULL_LINK;

// ---- port of it.unimi.dsi.fastutil.Arrays.quickSort(from, to, IntComparator, Swapper) ----
// Not stable: the order of equal-height nodes after sortBySize depends on this exact algorithm.
constexpr int QUICKSORT_NO_REC      = 16;
constexpr int QUICKSORT_MEDIAN_OF_9 = 128;

template <class Comp>
int med3(int a, int b, int c, Comp& comp) {
    const int ab = comp(a, b);
    const int ac = comp(a, c);
    const int bc = comp(b, c);
    return (ab < 0 ? (bc < 0 ? b : ac < 0 ? c : a) : (bc > 0 ? b : ac > 0 ? c : a));
}

template <class Swap>
void swapRange(Swap& swapper, int a, int b, int n) {
    for (int i = 0; i < n; i++, a++, b++) {
        swapper(a, b);
    }
}

template <class Comp, class Swap>
void quickSort(int from, int to, Comp& comp, Swap& swapper) {
    const int len = to - from;
    // Insertion sort on smallest arrays
    if (len < QUICKSORT_NO_REC) {
        for (int i = from; i < to; i++) {
            for (int j = i; j > from && (comp(j - 1, j) > 0); j--) {
                swapper(j, j - 1);
            }
        }
        return;
    }

    // Choose a partition element, v
    int m = from + len / 2;
    int l = from;
    int n = to - 1;
    if (len > QUICKSORT_MEDIAN_OF_9) { // Big arrays, pseudomedian of 9
        const int s = len / 8;
        l           = med3(l, l + s, l + 2 * s, comp);
        m           = med3(m - s, m, m + s, comp);
        n           = med3(n - 2 * s, n - s, n, comp);
    }
    m = med3(l, m, n, comp); // Mid-size, med of 3

    int a = from;
    int b = a;
    int c = to - 1;
    // Establish Invariant: v* (<v)* (>v)* v*
    int d = c;
    for (;;) {
        int comparison;
        while (b <= c && ((comparison = comp(b, m)) <= 0)) {
            if (comparison == 0) {
                if (a == m) {
                    m = b; // moving target; DELTA to JDK !!!
                } else if (b == m) {
                    m = a; // moving target; DELTA to JDK !!!
                }
                swapper(a++, b);
            }
            b++;
        }
        while (c >= b && ((comparison = comp(c, m)) >= 0)) {
            if (comparison == 0) {
                if (c == m) {
                    m = d; // moving target; DELTA to JDK !!!
                } else if (d == m) {
                    m = c; // moving target; DELTA to JDK !!!
                }
                swapper(c, d--);
            }
            c--;
        }
        if (b > c) {
            break;
        }
        if (b == m) {
            m = d; // moving target; DELTA to JDK !!!
        } else if (c == m) {
            m = c; // moving target; DELTA to JDK !!!
        }
        swapper(b++, c--);
    }

    // Swap partition elements back to middle
    int s;
    s = std::min(a - from, b - a);
    swapRange(swapper, from, b - s, s);
    s = std::min(d - c, to - d - 1);
    swapRange(swapper, b, to - s, s);

    // Recursively sort non-partition-elements
    if ((s = b - a) > 1) {
        quickSort(from, from + s, comp, swapper);
    }
    if ((s = d - c) > 1) {
        quickSort(to - s, to, comp, swapper);
    }
}

} // namespace

// ---- helpers ----

bool ReducingList::isDataVisible(std::uint64_t data) { return isAlphaVisible(rdp::alpha(data)); }

int ReducingList::getMinY(int i) const { return rdp::yMin(getData(i)); }
int ReducingList::getMaxY(int i) const { return rdp::yMax(getData(i)); }
int ReducingList::getSize(int i) const {
    const std::uint64_t d = getData(i);
    return rdp::yMax(d) - rdp::yMin(d);
}
int ReducingList::getAlpha(int i) const { return rdp::alpha(getData(i)); }

void ReducingList::setLink(int index, int shift, int value) {
    std::uint64_t& l = links_[idx(index)];
    l                = (l & ~(static_cast<std::uint64_t>(LINK_MASK) << shift))
      | (static_cast<std::uint64_t>(value & LINK_MASK) << shift);
}

void ReducingList::setMinY(int i, int minY) { data_[idx(i)] = rdp::setYMin(data_[idx(i)], minY); }

void ReducingList::clearEmpty() {
    setLowest(kNull);
    setHighest(kNull);
    setSmallest(kNull);
    setBiggest(kNull);
    setSizeWithAir(0);
    setSizeWithoutAir(0);
}

// ---- populate ----

bool ReducingList::populate(const ColumnRenderView& view) {
    const int size = view.size;
    if (size <= 0) {
        clearEmpty();
        links_.clear();
        data_.clear();
        sortingArray_.clear();
        return true;
    }

    // Room for (2 * size - 1) nodes: no input nodes touch, so an air node sits between every pair.
    const std::size_t arrayCapacity = (static_cast<std::size_t>(size) << 1) - 1;
    sortingArray_.assign(arrayCapacity, 0);
    links_.assign(arrayCapacity, DEFAULT_LINKS);
    data_.assign(arrayCapacity, DEFAULT_DATA);

    int sizeWithoutAir = 0;
    for (int index = 0; index < size; index++) {
        const std::uint64_t packedData = view.get(index);
        // skips nodes with 0 height and nodes that aren't visible; air nodes are inserted after sorting
        if (isDataVisible(packedData) && rdp::yMin(packedData) < rdp::yMax(packedData)) {
            setData(sizeWithoutAir, packedData);
            setSortingIndex(sizeWithoutAir, sizeWithoutAir);
            sizeWithoutAir++;
        }
    }

    // All air/invisible: the list stays empty. (Java leaves the size fields stale here; the
    // observable result of reduce/copyTo is identical, a void column.)
    if (sizeWithoutAir == 0) {
        clearEmpty();
        return true;
    }

    sortByPosition(sizeWithoutAir);
    int sizeWithAir = sizeWithoutAir;
    for (int sortingIndex = 1; sortingIndex < sizeWithoutAir; sortingIndex++) {
        const int           lowerIndex  = getSortingIndex(sortingIndex - 1);
        const int           higherIndex = getSortingIndex(sortingIndex);
        const std::uint64_t lowerData   = getData(lowerIndex);
        const std::uint64_t higherData  = getData(higherIndex);
        const int           lowerMaxY   = rdp::yMax(lowerData);
        const int           higherMinY  = rdp::yMin(higherData);

        if (lowerMaxY == higherMinY) {
            // the two nodes touch
            setHigher(lowerIndex, higherIndex);
            setLower(higherIndex, lowerIndex);
        } else if (lowerMaxY < higherMinY) {
            // the two nodes do not touch: insert an air node
            setData(
                sizeWithAir,
                rdp::create(
                    0,
                    0,
                    0,
                    0,
                    higherMinY,
                    lowerMaxY,
                    rdp::skyLight(higherData),
                    rdp::blockLight(higherData),
                    rdp::material(higherData)
                )
            );
            setSortingIndex(sizeWithAir, sizeWithAir);
            setLower(higherIndex, sizeWithAir);
            setHigher(lowerIndex, sizeWithAir);
            setLower(sizeWithAir, lowerIndex);
            setHigher(sizeWithAir, higherIndex);
            sizeWithAir++;
        } else {
            // the two nodes overlap (Java: IllegalArgumentException)
            clearEmpty();
            return false;
        }
    }
    lowest_  = sortingArray_[0];
    highest_ = sortingArray_[idx(sizeWithoutAir - 1)];

    // now sort by size
    sortBySize(sizeWithAir);
    for (int sortingIndex = 1; sortingIndex < sizeWithAir; sortingIndex++) {
        const int smallerIndex = getSortingIndex(sortingIndex - 1);
        const int biggerIndex  = getSortingIndex(sortingIndex);
        setBigger(smallerIndex, biggerIndex);
        setSmaller(biggerIndex, smallerIndex);
    }

    smallest_ = sortingArray_[0];
    biggest_  = sortingArray_[idx(sizeWithAir - 1)];

    setSizeWithAir(sizeWithAir);
    setSizeWithoutAir(sizeWithoutAir);
    return true;
}

// ---- reduce ----

void ReducingList::reduce(int target) {
    if (mergeVerySmallConnectedSegments(target)) {
        return;
    }
    if (mergeConnectedSegments(target)) {
        return;
    }
    if (removeLeastImportantSegments(target)) {
        return;
    }
    forceBottomToMerge(target);
}

bool ReducingList::checkLinks() const {
    if (getSizeWithoutAir() > getSizeWithAir()) {
        return false;
    }
    if (getSizeWithAir() == 0) {
        return getSmallest() == kNull && getBiggest() == kNull && getLowest() == kNull && getHighest() == kNull;
    }
    const int limit          = static_cast<int>(links_.size());
    int       sizeWithAir    = 0;
    int       sizeWithoutAir = 0;
    for (int index = getSmallest(); index != kNull; index = getBigger(index)) {
        if (index >= limit || sizeWithAir > limit) {
            return false;
        }
        const int smaller = getSmaller(index);
        const int bigger  = getBigger(index);
        if ((smaller != kNull ? getBigger(smaller) : getSmallest()) != index) {
            return false;
        }
        if ((bigger != kNull ? getSmaller(bigger) : getBiggest()) != index) {
            return false;
        }
        if (smaller != kNull && getSize(index) < getSize(smaller)) {
            return false;
        }
        sizeWithAir++;
        if (isIndexVisible(index)) {
            sizeWithoutAir++;
        }
    }
    if (sizeWithAir != getSizeWithAir() || sizeWithoutAir != getSizeWithoutAir()) {
        return false;
    }

    sizeWithAir = sizeWithoutAir = 0;
    for (int index = getLowest(); index != kNull; index = getHigher(index)) {
        if (index >= limit || sizeWithAir > limit) {
            return false;
        }
        const int lower  = getLower(index);
        const int higher = getHigher(index);
        if ((lower != kNull ? getHigher(lower) : getLowest()) != index) {
            return false;
        }
        if ((higher != kNull ? getLower(higher) : getHighest()) != index) {
            return false;
        }
        if (getMaxY(index) <= getMinY(index)) {
            return false;
        }
        if (lower != kNull && getMinY(index) != getMaxY(lower)) {
            return false;
        }
        sizeWithAir++;
        if (isIndexVisible(index)) {
            sizeWithoutAir++;
        }
    }
    return sizeWithAir == getSizeWithAir() && sizeWithoutAir == getSizeWithoutAir();
}

void ReducingList::remove(int index) {
    const int lower   = getLower(index);
    const int higher  = getHigher(index);
    const int smaller = getSmaller(index);
    const int bigger  = getBigger(index);
    const int alpha   = getAlpha(index);

    if (lower != kNull) {
        setHigher(lower, higher);
    } else {
        setLowest(higher);
    }

    if (higher != kNull) {
        setLower(higher, lower);
    } else {
        setHighest(lower);
    }

    if (smaller != kNull) {
        setBigger(smaller, bigger);
    } else {
        setSmallest(bigger);
    }

    if (bigger != kNull) {
        setSmaller(bigger, smaller);
    } else {
        setBiggest(smaller);
    }

    setData(index, DEFAULT_DATA);
    links_[idx(index)] = DEFAULT_LINKS;
    sizeWithAir_       = static_cast<std::uint16_t>(sizeWithAir_ - 1);

    if (isAlphaVisible(alpha)) {
        sizeWithoutAir_ = static_cast<std::uint16_t>(sizeWithoutAir_ - 1);
    }
}

void ReducingList::sortBySizeAndReLink() {
    if (getSizeWithAir() <= 1) {
        return;
    }

    int writeIndex = 0;
    for (int readIndex = getLowest(); readIndex != kNull; readIndex = getHigher(readIndex)) {
        if (data_[idx(readIndex)] != DEFAULT_DATA) {
            setSortingIndex(writeIndex++, readIndex);
        }
    }

    sortBySize(writeIndex);
    for (int index = 1; index < writeIndex; index++) {
        const int smaller = getSortingIndex(index - 1);
        const int bigger  = getSortingIndex(index);
        setSmaller(bigger, smaller);
        setBigger(smaller, bigger);
    }

    if (writeIndex == 0) {
        // Java: malformed (all-zero) data would index -1 below, so it forces writeIndex to 1.
        writeIndex = 1;
    }

    smallest_ = sortingArray_[0];
    biggest_  = sortingArray_[idx(writeIndex - 1)];
    setSmaller(getSmallest(), kNull);
    setBigger(getBiggest(), kNull);
}

void ReducingList::sortBySize(int size) {
    auto comp = [this](int index1, int index2) {
        const int a = getSize(getSortingIndex(index1));
        const int b = getSize(getSortingIndex(index2));
        return a < b ? -1 : (a == b ? 0 : 1); // Integer.compare
    };
    auto swapper = [this](int index1, int index2) {
        std::swap(sortingArray_[idx(index1)], sortingArray_[idx(index2)]);
    };
    quickSort(0, size, comp, swapper);
}

void ReducingList::sortByPosition(int size) {
    auto comp = [this](int index1, int index2) {
        const int a = getMinY(getSortingIndex(index1));
        const int b = getMinY(getSortingIndex(index2));
        return a < b ? -1 : (a == b ? 0 : 1);
    };
    auto swapper = [this](int index1, int index2) {
        std::swap(sortingArray_[idx(index1)], sortingArray_[idx(index2)]);
    };
    quickSort(0, size, comp, swapper);
}

void ReducingList::resortSize(int smaller) {
    int bigger = getBigger(smaller);

    // check if the node needs to be moved at all
    if (bigger == kNull || getSize(smaller) <= getSize(bigger)) {
        return;
    }

    // first remove smaller from before bigger
    int smallest = getSmaller(smaller);
    if (smallest != kNull) {
        setBigger(smallest, bigger);
    } else {
        setSmallest(bigger);
    }
    setSmaller(bigger, smallest);

    // next, find the position to re-insert the node
    do {
        bigger = getBigger(bigger);
    } while (bigger != kNull && getSize(smaller) > getSize(bigger));

    // lastly, re-insert the node where it belongs
    setSmaller(smaller, bigger != kNull ? getSmaller(bigger) : getBiggest());
    setBigger(smaller, bigger);

    if (bigger != kNull) {
        setSmaller(bigger, smaller);
    } else {
        setBiggest(smaller);
    }

    smallest = getSmaller(smaller);
    if (smallest != kNull) {
        setBigger(smallest, smaller);
    } else {
        setSmallest(smaller);
    }
}

int ReducingList::tryMergeStep1(int current, bool fastPath) {
    int       result = fastPath ? getSmaller(current) : getBigger(current);
    const int higher = getHigher(current);
    const int lower  = getLower(current);
    int       toExtendDownwards;
    int       toRemove;

    if (higher != kNull && getAlpha(higher) == getAlpha(current)) {
        if (lower != kNull && getAlpha(lower) == getAlpha(current)) {
            if (getSize(higher) <= getSize(lower)) {
                toExtendDownwards = higher;
                toRemove          = current;
            } else {
                toExtendDownwards = current;
                toRemove          = lower;
            }
        } else {
            toExtendDownwards = higher;
            toRemove          = current;
        }
    } else {
        if (lower != kNull && getAlpha(lower) == getAlpha(current)) {
            toExtendDownwards = current;
            toRemove          = lower;
        } else {
            return result;
        }
    }

    // if we're about to remove the next node for iteration, continue at the node after that
    if (result == toRemove) {
        result = getSmaller(result);
    }

    setMinY(toExtendDownwards, getMinY(toRemove));
    if (!fastPath) {
        resortSize(toExtendDownwards);
    }

    remove(toRemove);

    // off the fast path the list changed under the step 1 loop, so it starts over
    return fastPath ? result : getSmallest();
}

int ReducingList::lowerNode(int size) const {
    for (int node = getSmallest(); node != kNull; node = getBigger(node)) {
        if (getSize(node) >= size) {
            return getSmaller(node);
        }
    }
    return getBiggest();
}

bool ReducingList::mergeVerySmallConnectedSegments(int target) {
    for (int specialCase = 1; specialCase <= SPECIAL_CASES; specialCase++) {
        for (int current = lowerNode(specialCase + 1); current != kNull;) {
            if (getSizeWithoutAir() <= target) {
                sortBySizeAndReLink();
                return true;
            }
            current = tryMergeStep1(current, true);
        }
        sortBySizeAndReLink();
    }
    return false;
}

bool ReducingList::mergeConnectedSegments(int target) {
    for (int current = getSmallest(); current != kNull;) {
        if (getSizeWithoutAir() <= target) {
            return true;
        }
        current = tryMergeStep1(current, false);
    }
    return false;
}

bool ReducingList::removeLeastImportantSegments(int target) {
    for (int center = getSmallest(); center != kNull;) {
        if (getSizeWithoutAir() <= target) {
            return true;
        }

        const int lower  = getLower(center);
        const int higher = getHigher(center);
        if (lower != kNull && higher != kNull && getAlpha(lower) == getAlpha(higher)) {
            setMinY(higher, getMinY(lower));
            resortSize(higher);
            remove(lower);
            remove(center);

            center = getSmallest();
        } else {
            center = getBigger(center);
        }
    }
    return false;
}

void ReducingList::forceBottomToMerge(int target) {
    for (int lowest = getLowest(); lowest != kNull;) {
        if (getSizeWithoutAir() <= target) {
            return;
        }

        const int lowY   = getMinY(lowest);
        int       higher = getHigher(lowest);
        while (true) {
            if (higher == kNull) {
                // target is 0 or negative
                clearEmpty();
                return;
            }

            // don't merge the lowest segment with an invisible one: eliminate the invisible
            // segment too and give the top visible segment the bottom one's min Y
            if (isIndexVisible(higher)) {
                setMinY(higher, lowY);
                resortSize(higher);
                remove(lowest);

                lowest = getLowest();
                break;
            }
            remove(lowest);
            lowest = higher;
            higher = getHigher(higher);
            // don't update lowY
        }
    }
}

std::uint64_t ReducingList::reduceToOne(const ColumnRenderView& view) {
    const int size = view.size;
    if (size <= 0) {
        return rdp::EMPTY_DATA;
    }

    std::uint64_t highestDataPoint = 0;
    std::uint64_t lowestDataPoint  = 0;
    int           index            = 0;
    // first loop: find the first visible segment
    bool found = false;
    for (; index < size; index++) {
        const std::uint64_t dataPoint = view.get(index);
        if (isDataVisible(dataPoint)) {
            highestDataPoint = dataPoint;
            lowestDataPoint  = dataPoint;
            found            = true;
            break;
        }
    }
    if (!found) {
        return rdp::EMPTY_DATA; // no visible segments
    }

    // second loop: merge the rest (note the else-if: a new highest is never also tested as lowest)
    for (; index < size; index++) {
        const std::uint64_t dataPoint = view.get(index);
        if (isDataVisible(dataPoint)) {
            const int yMax = rdp::yMax(dataPoint);
            const int yMin = rdp::yMin(dataPoint);

            if (yMax > rdp::yMax(highestDataPoint)) {
                highestDataPoint = dataPoint;
            } else if (yMin < rdp::yMin(lowestDataPoint)) {
                lowestDataPoint = dataPoint;
            }
        }
    }

    return rdp::setYMin(highestDataPoint, rdp::yMin(lowestDataPoint));
}

void ReducingList::copyTo(ColumnRenderView& view) const {
    // highest to lowest (DH breaks if this is reversed)
    int writeIndex = 0;
    for (int node = getHighest(); node != kNull; node = getLower(node)) {
        if (isIndexVisible(node) && writeIndex < view.size) {
            view.set(writeIndex++, getData(node));
        }
    }

    // the view must be non-empty: add a void data point if nothing was visible
    if (writeIndex == 0 && view.size > 0) {
        view.set(writeIndex++, rdp::EMPTY_DATA);
    }

    for (const int size = view.size; writeIndex < size; writeIndex++) {
        view.set(writeIndex, rdp::EMPTY_DATA);
    }
}

} // namespace dh
