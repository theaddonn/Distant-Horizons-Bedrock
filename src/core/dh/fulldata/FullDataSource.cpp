#include "dh/fulldata/FullDataSource.h"

#include <algorithm>

namespace dh {

std::vector<std::uint64_t>* FullDataSource::tryColumn(int x, int z) {
    const int i = tryRelativePosToIndex(x, z);
    return i < 0 ? nullptr : &columns[static_cast<std::size_t>(i)];
}

const std::vector<std::uint64_t>* FullDataSource::tryColumn(int x, int z) const {
    const int i = tryRelativePosToIndex(x, z);
    return i < 0 ? nullptr : &columns[static_cast<std::size_t>(i)];
}

void FullDataSource::setSingleColumn(std::vector<std::uint64_t> col, int x, int z, std::int8_t genStep, std::int8_t compMode) {
    const int i = tryRelativePosToIndex(x, z);
    if (i < 0) return;
    const auto idx = static_cast<std::size_t>(i);
    columns[idx] = std::move(col);
    columnGenerationStep[idx] = genStep;
    columnWorldCompressionMode[idx] = compMode;
}

bool FullDataSource::isColumnInWrongOrder(const std::vector<std::uint64_t>& col) {
    if (col.size() < 2) return false;
    return fdp::minY(col.front()) < fdp::minY(col.back());
}

void FullDataSource::ensureDataColumnOrder(std::vector<std::uint64_t>& col) {
    if (isColumnInWrongOrder(col)) std::reverse(col.begin(), col.end());
}

void FullDataSource::clearAllNonAdjData(AdjDirection dir) {
    const AdjRange r = adjRange(dir, WIDTH);
    for (int x = 0; x < WIDTH; ++x) {
        for (int z = 0; z < WIDTH; ++z) {
            if (x >= r.minX && x < r.maxX && z >= r.minZ && z < r.maxZ) continue;
            auto& col = column(x, z);
            col.clear();
            col.push_back(fdp::EMPTY_DATA_POINT);
        }
    }
}

FullDataSource FullDataSource::createEmpty(std::uint64_t pos) {
    FullDataSource s;
    s.pos = pos;
    s.mapping = IdMap(pos);
    s.isEmpty = true;
    return s;
}

namespace {
inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    // splitmix64-style combine; order sensitive
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}
} // namespace

std::uint64_t FullDataSource::contentHash() const {
    std::uint64_t h = mix(0x1234ABCDull, pos);
    for (const auto& col : columns) {
        h = mix(h, col.size());
        for (std::uint64_t v : col) h = mix(h, v);
    }
    for (std::int8_t v : columnGenerationStep) h = mix(h, static_cast<std::uint8_t>(v));
    h = mix(h, 0xFEEDull);
    for (std::int8_t v : columnWorldCompressionMode) h = mix(h, static_cast<std::uint8_t>(v));
    return h;
}

bool FullDataSource::contentEquals(const FullDataSource& o) const {
    return pos == o.pos && columns == o.columns && columnGenerationStep == o.columnGenerationStep &&
           columnWorldCompressionMode == o.columnWorldCompressionMode;
}

} // namespace dh
