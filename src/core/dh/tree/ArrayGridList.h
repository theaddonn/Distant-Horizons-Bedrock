#pragma once

// Port of util/gridList/ArrayGridList.java: a gridSize x gridSize grid stored row-major in a flat list.
//
// Differences to Java:
//   * Java throws IndexOutOfBoundsException / IllegalStateException in the sub-grid constructor; here
//     that is the static factory subGrid() which returns std::nullopt instead.
//   * ArrayGridList.fill(null dealloc, ...) would NPE in Java when a non-null item is replaced;
//     here a null `dealloc` is simply skipped.
//   * get/set outside the grid return null (T{}) and do nothing, as in Java.

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dh/tree/GridRing.h"

namespace dh::tree {

template <class T>
class ArrayGridList {
public:
    using Pos2D    = GridPos2D;
    using Filler   = std::function<T(std::int32_t, std::int32_t)>;
    using Dealloc  = std::function<void(T&)>;
    using PosVisit = std::function<void(std::int32_t, std::int32_t)>;

    const std::int32_t gridSize;

    /// @param filler called for each (x, y) (row-major, y outer) to produce the initial values; empty = all null
    explicit ArrayGridList(std::int32_t gridSize_, const Filler& filler = Filler{}) : gridSize(gridSize_) {
        assert(gridSize_ >= 0);
        data_.reserve(static_cast<std::size_t>(gridSize_) * static_cast<std::size_t>(gridSize_));
        // Java calls the (virtual) forEachPos from the super constructor, so subclass offsets are still 0 here.
        for (std::int32_t y = 0; y < gridSize; y++) {
            for (std::int32_t x = 0; x < gridSize; x++) {
                data_.push_back(filler ? filler(x, y) : T{});
            }
        }
    }
    ArrayGridList(const ArrayGridList&)            = default;
    ArrayGridList& operator=(const ArrayGridList&) = delete; // gridSize is const (Java's final field)
    virtual ~ArrayGridList()                       = default;

    /// Copy of the square [minR, maxR) x [minR, maxR) of `from` (raw indices). nullopt where Java throws.
    static std::optional<ArrayGridList> subGrid(const ArrayGridList& from, std::int32_t minR, std::int32_t maxR) {
        if (minR > maxR || minR < 0 || maxR > from.gridSize) {
            return std::nullopt;
        }
        ArrayGridList out(maxR - minR);
        out.data_.clear();
        for (std::int32_t oy = minR; oy < maxR; oy++) {
            const std::int32_t begin = minR + oy * from.gridSize;
            const std::int32_t end   = maxR + oy * from.gridSize;
            out.data_.insert(out.data_.end(), from.data_.begin() + begin, from.data_.begin() + end);
        }
        return out;
    }

    //=========//
    // methods //
    //=========//

    T get(const Pos2D& pos) const { return get(pos.x, pos.y); }
    T set(const Pos2D& pos, T e) { return set(pos.x, pos.y, std::move(e)); }
    /// null (T{}) if outside the grid
    T get(std::int32_t x, std::int32_t y) const {
        if (!inRange(x, y)) {
            return T{};
        }
        return data_[static_cast<std::size_t>(getIndexOf(x, y))];
    }
    T getFirst() const { return get(0, 0); }
    T getLast() const { return get(gridSize - 1, gridSize - 1); }

    /// @return the previous value; null (and nothing is set) if outside the grid
    T set(std::int32_t x, std::int32_t y, T e) {
        if (!inRange(x, y)) {
            return T{};
        }
        T& slot = data_[static_cast<std::size_t>(getIndexOf(x, y))];
        T  old  = std::move(slot);
        slot    = std::move(e);
        return old;
    }
    T setFirst(T e) { return set(0, 0, std::move(e)); }
    T setLast(T e) { return set(gridSize - 1, gridSize - 1, std::move(e)); }

    virtual bool inRange(std::int32_t x, std::int32_t y) const {
        return x >= 0 && x < gridSize && y >= 0 && y < gridSize;
    }

    void clear() { clear(Dealloc{}); }
    void fill(const Filler& filler) { fill(Dealloc{}, filler); }
    void clear(const Dealloc& dealloc) { fill(dealloc, Filler{}); }
    void fill(const Dealloc& dealloc, const Filler& filler) {
        forEachPos([&](std::int32_t x, std::int32_t y) {
            T t = set(x, y, filler ? filler(x, y) : T{});
            if (detail::isSet(t) && dealloc) {
                dealloc(t);
            }
        });
    }

    //===========//
    // iterators //
    //===========//

    /// y outer, x inner
    virtual void forEachPos(const PosVisit& consumer) const {
        for (std::int32_t y = 0; y < gridSize; y++) {
            for (std::int32_t x = 0; x < gridSize; x++) {
                consumer(x, y);
            }
        }
    }

    /// ArrayList-style raw access (row-major).
    std::size_t size() const { return data_.size(); }
    T&          getByIndex(std::size_t i) { return data_[i]; }
    const T&    getByIndex(std::size_t i) const { return data_[i]; }
    template <class F>
    void forEach(F&& consumer) {
        for (T& t : data_) {
            consumer(t);
        }
    }

    virtual std::string toString() const {
        return "ArrayGridList " + std::to_string(gridSize) + "*" + std::to_string(gridSize) + "["
             + std::to_string(data_.size()) + "]";
    }

protected:
    virtual std::int32_t getIndexOf(std::int32_t x, std::int32_t y) const { return x + y * gridSize; }

    std::vector<T> data_;
};

} // namespace dh::tree
