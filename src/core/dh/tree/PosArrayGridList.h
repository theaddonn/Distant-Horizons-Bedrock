#pragma once

// Port of util/gridList/PosArrayGridList.java: an ArrayGridList whose coordinates are shifted by (offsetX, offsetY),
// i.e. it covers [offset, offset + gridSize) on both axes. NOT thread safe (as in Java).
//
// Java quirks kept:
//   * The constructor runs the filler with 0-based coordinates (offsets are not assigned yet when the
//     base constructor fills the list), not with the offset coordinates.
//   * The sub-grid copy does not carry the offset (it is 0,0).

#include <cstdint>
#include <optional>
#include <string>

#include "dh/tree/ArrayGridList.h"

namespace dh::tree {

template <class T>
class PosArrayGridList : public ArrayGridList<T> {
    using Base = ArrayGridList<T>;

public:
    using typename Base::Dealloc;
    using typename Base::Filler;
    using typename Base::PosVisit;

    PosArrayGridList(std::int32_t gridSize, std::int32_t offsetX, std::int32_t offsetY, const Filler& filler = Filler{})
    : Base(gridSize, filler),
      offsetX_(offsetX),
      offsetY_(offsetY) {}

    /// Copy of `source`'s `[minR, maxR)` square with offsets (0, 0); nullopt where Java throws.
    static std::optional<PosArrayGridList>
    subGrid(const PosArrayGridList& source, std::int32_t minR, std::int32_t maxR) {
        auto base = Base::subGrid(source, minR, maxR);
        if (!base) {
            return std::nullopt;
        }
        return PosArrayGridList(*base);
    }

    std::int32_t getOffsetX() const { return offsetX_; }
    std::int32_t getOffsetY() const { return offsetY_; }

    bool inRange(std::int32_t x, std::int32_t y) const override {
        return x >= offsetX_ && x < wadd(offsetX_, this->gridSize) && y >= offsetY_
            && y < wadd(offsetY_, this->gridSize);
    }

    void forEachPos(const PosVisit& consumer) const override {
        for (std::int32_t y = offsetY_; y < wadd(offsetY_, this->gridSize); y++) {
            for (std::int32_t x = offsetX_; x < wadd(offsetX_, this->gridSize); x++) {
                consumer(x, y);
            }
        }
    }

    /// Shifts the covered window by (deltaX, deltaY), keeping the overlapping items.
    /// @return false if nothing changed
    bool move(std::int32_t deltaX, std::int32_t deltaY, const Dealloc& dealloc) {
        if (deltaX == 0 && deltaY == 0) {
            return false;
        }

        // moved by >= the whole width: drop everything and just update the offset
        if (jabs32(deltaX) >= this->gridSize || jabs32(deltaY) >= this->gridSize) {
            this->clear(dealloc);
            offsetX_ = wadd(offsetX_, deltaX);
            offsetY_ = wadd(offsetY_, deltaY);
            return true;
        }
        const std::int32_t newMinX = wadd(offsetX_, deltaX);
        const std::int32_t newMinY = wadd(offsetY_, deltaY);
        const std::int32_t newMaxX = wadd(newMinX, this->gridSize);
        const std::int32_t newMaxY = wadd(newMinY, this->gridSize);

        if (dealloc) {
            this->forEachPos([&](std::int32_t x, std::int32_t y) {
                if (x < newMinX || y < newMinY || x >= newMaxX || y >= newMaxY) {
                    T t = this->get(x, y);
                    if (detail::isSet(t)) {
                        dealloc(t);
                    }
                }
            });
        }

        offsetX_ = newMinX;
        offsetY_ = newMinY;

        if (deltaX >= 0 && deltaY >= 0) {
            for (std::int32_t x = newMinX; x < newMaxX; x++) {
                for (std::int32_t y = newMinY; y < newMaxY; y++) {
                    this->set(x, y, directGet(wadd(x, deltaX), wadd(y, deltaY)));
                }
            }
        } else if (deltaX < 0 && deltaY >= 0) {
            for (std::int32_t x = newMaxX - 1; x >= newMinX; x--) {
                for (std::int32_t y = newMinY; y < newMaxY; y++) {
                    this->set(x, y, directGet(wadd(x, deltaX), wadd(y, deltaY)));
                }
            }
        } else if (deltaX >= 0) { // && deltaY < 0
            for (std::int32_t x = newMinX; x < newMaxX; x++) {
                for (std::int32_t y = newMaxY - 1; y >= newMinY; y--) {
                    this->set(x, y, directGet(wadd(x, deltaX), wadd(y, deltaY)));
                }
            }
        } else { // deltaX < 0 && deltaY < 0
            for (std::int32_t x = newMaxX - 1; x >= newMinX; x--) {
                for (std::int32_t y = newMaxY - 1; y >= newMinY; y--) {
                    this->set(x, y, directGet(wadd(x, deltaX), wadd(y, deltaY)));
                }
            }
        }
        return true;
    }

    std::string toString() const override {
        return "PosArrayGridList[" + std::to_string(offsetX_) + "," + std::to_string(offsetY_) + "] "
             + std::to_string(this->gridSize) + "*" + std::to_string(this->gridSize) + "["
             + std::to_string(this->size()) + "]";
    }

protected:
    std::int32_t getIndexOf(std::int32_t x, std::int32_t y) const override {
        return wadd(wsub(x, offsetX_), wmul(wsub(y, offsetY_), this->gridSize));
    }

private:
    explicit PosArrayGridList(const Base& base) : Base(base) {}

    T directGet(std::int32_t x, std::int32_t y) const { return inRange(x, y) ? this->get(x, y) : T{}; }

    std::int32_t offsetX_ = 0;
    std::int32_t offsetY_ = 0;
};

} // namespace dh::tree
