#pragma once

// Port of util/gridList/MovableGridRingList.java.
//
// A fixed-size (2*halfWidth+1)^2 grid of T that "slides" over an unbounded 2D integer plane: the grid
// covers [min, min+width) on both axes and cell (x, y) lives in storage slot
// floorMod(x, width) + floorMod(y, width) * width, so moving the centre only has to clear the cells that
// fall out of range instead of copying everything.
//
// Differences to Java:
//   * Java's class extends ArrayList<T>; here the raw storage is exposed through size()/begin()/end()/forEach()
//     and iterates in STORAGE order (not ring order), exactly like iterating the Java ArrayList.
//   * "null" is the default constructed T. For pointer-like T (raw/smart pointers, optional) "set" means
//     non-null / has_value; for any other T every cell counts as set.
//   * No ReentrantReadWriteLock: this port is single threaded (WP-G2 decides how to guard it).
//   * get/set/swap outside the grid return null/false exactly like Java (nothing throws here).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "dh/util/Bits.h"

namespace dh::tree {

/// MovableGridRingList.Pos2D (immutable int pair).
struct GridPos2D {
    std::int32_t x = 0, y = 0;

    constexpr GridPos2D() = default;
    constexpr GridPos2D(std::int32_t x_, std::int32_t y_) : x(x_), y(y_) {}
    constexpr bool operator==(const GridPos2D&) const = default;

    constexpr std::int32_t getX() const { return x; }
    constexpr std::int32_t getY() const { return y; }

    constexpr GridPos2D add(const GridPos2D& o) const { return {wadd(x, o.x), wadd(y, o.y)}; }
    constexpr GridPos2D subtract(const GridPos2D& o) const { return {wsub(x, o.x), wsub(y, o.y)}; }
    constexpr GridPos2D subtract(std::int32_t v) const { return {wsub(x, v), wsub(y, v)}; }

    /// sqrt(pow(x - ox, 2) + pow(y - oy, 2)) with the int difference wrapping first.
    double dist(const GridPos2D& o) const {
        const double dx = static_cast<double>(wsub(x, o.x));
        const double dy = static_cast<double>(wsub(y, o.y));
        return std::sqrt(dx * dx + dy * dy);
    }
    constexpr std::int64_t distSquared(const GridPos2D& o) const {
        const std::int64_t dx = static_cast<std::int64_t>(x) - o.x;
        const std::int64_t dy = static_cast<std::int64_t>(y) - o.y;
        return wadd64(wmul64(dx, dx), wmul64(dy, dy));
    }
    constexpr std::int32_t chebyshevDist(const GridPos2D& o) const {
        const std::int32_t a = jabs32(wsub(x, o.x));
        const std::int32_t b = jabs32(wsub(y, o.y));
        return a > b ? a : b;
    }
    constexpr std::int32_t manhattanDist(const GridPos2D& o) const {
        return wadd(jabs32(wsub(x, o.x)), jabs32(wsub(y, o.y)));
    }

    /// Objects.hash(x, y)
    constexpr std::int32_t hashCode() const { return wadd(wmul(wadd(31, x), 31), y); }
    /// "[x, y]"
    std::string toString() const { return "[" + std::to_string(x) + ", " + std::to_string(y) + "]"; }
};

namespace detail {
/// Java `item != null` for the grid element type (see header comment).
template <class T>
constexpr bool isSet(const T& v) {
    if constexpr (requires { v == nullptr; }) {
        return !(v == nullptr);
    } else if constexpr (requires { v.has_value(); }) {
        return v.has_value();
    } else {
        return true;
    }
}
} // namespace detail

template <class T>
class MovableGridRingList {
public:
    using Pos2D          = GridPos2D;
    using Consumer       = std::function<void(T&)>;
    using PosConsumer    = std::function<void(const Pos2D&, T&)>;
    using iterator       = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    MovableGridRingList(std::int32_t halfWidth, std::int32_t centerX, std::int32_t centerY)
    : width_(wadd(wmul(halfWidth, 2), 1)),
      halfWidth_(halfWidth),
      min_(wsub(centerX, halfWidth), wsub(centerY, halfWidth)) {
        assert(halfWidth >= 0);
        ringPositions_ = createRingIteratorList();
        clear();
    }

    //=====================//
    // getters and setters //
    //=====================//

    T get(const Pos2D& pos) const { return get(pos.x, pos.y); }
    /// Returns null (T{}) if x,y is outside the grid.
    T get(std::int32_t x, std::int32_t y) const {
        if (!inRange(x, y)) {
            return T{};
        }
        return data_[index(x, y)];
    }
    /// Pointer to the cell, nullptr if x,y is outside the grid (the cell content may still be null).
    T*       getPtr(std::int32_t x, std::int32_t y) { return inRange(x, y) ? &data_[index(x, y)] : nullptr; }
    const T* getPtr(std::int32_t x, std::int32_t y) const { return inRange(x, y) ? &data_[index(x, y)] : nullptr; }

    bool set(const Pos2D& pos, T item) { return set(pos.x, pos.y, std::move(item)); }
    /// Returns false (and does nothing) if x,y is outside the grid.
    bool set(std::int32_t x, std::int32_t y, T item) {
        if (!inRange(x, y)) {
            return false;
        }
        data_[index(x, y)] = std::move(item);
        return true;
    }

    //================//
    // list modifiers //
    //================//

    T swap(const Pos2D& pos, T item) { return swap(pos.x, pos.y, std::move(item)); }
    /// Puts `item` at x,y and returns the previous content; returns the input item if x,y is outside the grid.
    T swap(std::int32_t x, std::int32_t y, T item) {
        if (!inRange(x, y)) {
            return item;
        }
        return swapUnsafe(x, y, std::move(item));
    }

    T remove(const Pos2D& pos) { return remove(pos.x, pos.y); }
    /// Removes and returns the item at x,y; null if x,y is outside the grid.
    T remove(std::int32_t x, std::int32_t y) { return swap(x, y, T{}); }

    void clear() { clear(Consumer{}); }
    /// @param removedItemConsumer called (storage order) on each non-null item before it is removed.
    void clear(const Consumer& removedItemConsumer) {
        if (removedItemConsumer) {
            for (T& item : data_) {
                if (detail::isSet(item)) {
                    removedItemConsumer(item);
                }
            }
        }
        data_.clear();
        data_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(width_));
    }

    bool moveTo(std::int32_t newCenterX, std::int32_t newCenterY) { return moveTo(newCenterX, newCenterY, Consumer{}); }
    bool moveTo(std::int32_t newCenterX, std::int32_t newCenterY, const Consumer& removedItemConsumer) {
        return moveTo(newCenterX, newCenterY, removedItemConsumer, PosConsumer{});
    }
    /// @return true if the grid moved. removedItemConsumer is only called for non-null removed items;
    ///         nullableRemovedItemConsumer is called for EVERY cell (kept cells get a null item).
    bool moveTo(
        std::int32_t       newCenterX,
        std::int32_t       newCenterY,
        const Consumer&    removedItemConsumer,
        const PosConsumer& nullableRemovedItemConsumer
    ) {
        const Pos2D        cPos    = min_;
        const std::int32_t newMinX = wsub(newCenterX, halfWidth_);
        const std::int32_t newMinY = wsub(newCenterY, halfWidth_);
        if (cPos.x == newMinX && cPos.y == newMinY) {
            return false;
        }

        const std::int32_t deltaX = wsub(newMinX, cPos.x);
        const std::int32_t deltaY = wsub(newMinY, cPos.y);

        // if the offset is >= the total width, just delete everything and update the pos
        if (jabs32(deltaX) >= width_ || jabs32(deltaY) >= width_) {
            clear(removedItemConsumer);
        } else {
            for (std::int32_t x = 0; x < width_; x++) {
                for (std::int32_t y = 0; y < width_; y++) {
                    const Pos2D itemPos(wadd(x, cPos.x), wadd(y, cPos.y));

                    if (x - deltaX < 0 || y - deltaY < 0 || x - deltaX >= width_ || y - deltaY >= width_) {
                        T item = swapUnsafe(itemPos.x, itemPos.y, T{});
                        if (detail::isSet(item) && removedItemConsumer) {
                            removedItemConsumer(item);
                        }
                        if (nullableRemovedItemConsumer) {
                            nullableRemovedItemConsumer(itemPos, item);
                        }
                    } else if (nullableRemovedItemConsumer) {
                        T none{};
                        nullableRemovedItemConsumer(itemPos, none);
                    }
                }
            }
        }

        min_ = Pos2D(newMinX, newMinY);
        return true;
    }

    //==================//
    // position getters //
    //==================//

    Pos2D        getCenter() const { return Pos2D(wadd(min_.x, halfWidth_), wadd(min_.y, halfWidth_)); }
    Pos2D        getMinPosInRange() const { return min_; }
    Pos2D        getMaxPosInRange() const { return Pos2D(wadd(min_.x, width_ - 1), wadd(min_.y, width_ - 1)); }
    std::int32_t getWidth() const { return width_; }
    std::int32_t getHalfWidth() const { return halfWidth_; }

    bool inRange(std::int32_t x, std::int32_t y) const {
        return x >= min_.x && x < wadd(min_.x, width_) && y >= min_.y && y < wadd(min_.y, width_);
    }

    //===========//
    // iterators //
    //===========//

    /// ArrayList-style access to the raw storage (storage order, includes null cells).
    std::size_t    size() const { return data_.size(); }
    iterator       begin() { return data_.begin(); }
    iterator       end() { return data_.end(); }
    const_iterator begin() const { return data_.begin(); }
    const_iterator end() const { return data_.end(); }
    /// ArrayList.forEach: storage order, passes null cells too.
    template <class F>
    void forEach(F&& consumer) {
        for (T& item : data_) {
            consumer(item);
        }
    }

    /// x outer, y inner over the covered range; passes null entries. consumer(T&, Pos2D)
    template <class F>
    void forEachPos(F&& consumer) {
        for (std::int32_t x = min_.x; x < wadd(min_.x, width_); x++) {
            for (std::int32_t y = min_.y; y < wadd(min_.y, width_); y++) {
                consumer(data_[index(x, y)], Pos2D(x, y));
            }
        }
    }

    /// In-to-out order (nearest the centre first); SKIPS null entries. consumer(T&)
    template <class F>
    void forEachOrdered(F&& consumer) {
        for (const Pos2D& offset : ringPositions_) {
            T& item = data_[index(wadd(min_.x, offset.x), wadd(min_.y, offset.y))];
            if (detail::isSet(item)) {
                consumer(item);
            }
        }
    }

    /// In-to-out order; passes null entries. consumer(T&, Pos2D)
    template <class F>
    void forEachPosOrdered(F&& consumer) {
        for (const Pos2D& offset : ringPositions_) {
            const std::int32_t x = wadd(min_.x, offset.x);
            const std::int32_t y = wadd(min_.y, offset.y);
            assert(inRange(x, y));
            consumer(data_[index(x, y)], Pos2D(x, y));
        }
    }

    //==============//
    // base methods //
    //==============//

    /// "MovableGridRingList[cx,cy] w*w[size]"
    std::string toString() const {
        return "MovableGridRingList[" + std::to_string(wadd(min_.x, halfWidth_)) + ","
             + std::to_string(wadd(min_.y, halfWidth_)) + "] " + std::to_string(width_) + "*" + std::to_string(width_)
             + "[" + std::to_string(data_.size()) + "]";
    }

private:
    std::size_t index(std::int32_t x, std::int32_t y) const {
        return static_cast<std::size_t>(jfloormod(x, width_) + wmul(jfloormod(y, width_), width_));
    }
    T swapUnsafe(std::int32_t x, std::int32_t y, T item) {
        T& slot = data_[index(x, y)];
        T  old  = std::move(slot);
        slot    = std::move(item);
        return old;
    }

    std::vector<Pos2D> createRingIteratorList() const {
        std::vector<Pos2D> posArray;
        posArray.reserve(static_cast<std::size_t>(width_) * static_cast<std::size_t>(width_));
        for (std::int32_t xPos = -halfWidth_; xPos <= halfWidth_; xPos++) {
            for (std::int32_t zPos = -halfWidth_; zPos <= halfWidth_; zPos++) {
                posArray.emplace_back(xPos, zPos);
            }
        }
        // Arrays.sort on objects is stable: nearest to the origin first, ties keep the x-major order above.
        std::stable_sort(posArray.begin(), posArray.end(), [](const Pos2D& a, const Pos2D& b) {
            const std::int64_t disSqrA = static_cast<std::int64_t>(a.x) * a.x + static_cast<std::int64_t>(a.y) * a.y;
            const std::int64_t disSqrB = static_cast<std::int64_t>(b.x) * b.x + static_cast<std::int64_t>(b.y) * b.y;
            return static_cast<double>(disSqrA) < static_cast<double>(disSqrB);
        });
        const Pos2D halfPos(halfWidth_, halfWidth_);
        for (Pos2D& p : posArray) {
            p = p.add(halfPos);
        }
        return posArray;
    }

    std::int32_t       width_;
    std::int32_t       halfWidth_;
    Pos2D              min_; // the position of this grid closest to negative x/z infinity
    std::vector<Pos2D> ringPositions_;
    std::vector<T>     data_;
};

} // namespace dh::tree
