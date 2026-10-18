#include "dh/storage/Codec.h"

#include "dh/fulldata/Enums.h"
#include "dh/fulldata/FullDataPoint.h"
#include "dh/platform/ILogger.h"
#include "dh/util/Varint.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dh {

namespace {

using Bytes = std::vector<std::byte>;
using View  = std::span<const std::byte>;

constexpr std::uint32_t kMaxColumnLen = 4096;     // heights are 12 bit
constexpr std::uint32_t kMaxPoints    = 1u << 22; // per blob; real sections hold ~10^4-10^5
constexpr std::uint32_t kMaxMapping   = 1u << 24; // ids are shifted by 2 into a 32 bit varint
constexpr int           kInteriorMin  = 1;
constexpr int           kInteriorMax  = FullDataSource::WIDTH - 1; // exclusive

struct Region {
    int minX, maxX, minZ, maxZ;
    int columnCount() const { return (maxX - minX) * (maxZ - minZ); }
};

constexpr Region kInterior{kInteriorMin, kInteriorMax, kInteriorMin, kInteriorMax};

Region stripRegion(AdjDirection d) {
    const AdjRange r = adjRange(d, FullDataSource::WIDTH);
    return Region{r.minX, r.maxX, r.minZ, r.maxZ};
}

Error corrupt(std::string_view what) { return makeError(ErrorCode::Corrupt, what); }
Error invalid(std::string_view what) { return makeError(ErrorCode::InvalidArgument, what); }

void putVar(Bytes& out, std::uint32_t v) { (void)varint::writeVarint(out, v); }

// ---------------------------------------------------------------- framing

Result<Bytes> frame(const Bytes& payload, const CodecOptions& opt) {
    if (payload.size() > kCodecMaxBlobBytes) return invalid("codec: blob too large");
    if (opt.compression != Compression::None) {
        // TODO(WP-D1b): zstd. Framing: [ver][1][varint rawSize][zstd frame].
        return makeError(ErrorCode::Unsupported, "codec: zstd compression not available yet");
    }
    Bytes out;
    out.reserve(payload.size() + 2);
    out.push_back(static_cast<std::byte>(kCodecFormatVersion));
    out.push_back(static_cast<std::byte>(Compression::None));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// `view` points into `blob` (None) or into `storage` (decompressed).
Status unframe(const Bytes& blob, Bytes& storage, View& view) {
    (void)storage;
    if (blob.size() < 2) return corrupt("codec: blob shorter than header");
    if (blob.size() > kCodecMaxBlobBytes) return corrupt("codec: blob exceeds sanity cap");
    if (static_cast<std::uint8_t>(blob[0]) != kCodecFormatVersion)
        return makeError(ErrorCode::Unsupported, "codec: unknown format version");
    const auto comp = static_cast<std::uint8_t>(blob[1]);
    if (comp == static_cast<std::uint8_t>(Compression::None)) {
        view = View(blob).subspan(2);
        return Status::ok();
    }
    if (comp == static_cast<std::uint8_t>(Compression::Zstd))
        return makeError(ErrorCode::Unsupported, "codec: zstd blobs not supported by this build");
    return corrupt("codec: unknown compression id");
}

std::uint32_t fnvUpdate(std::uint32_t h, View data) {
    for (std::byte b : data) {
        h ^= static_cast<std::uint8_t>(b);
        h *= 16777619u;
    }
    return h;
}

std::uint32_t checksumOf(const View (&payloads)[8]) {
    std::uint32_t h = 2166136261u;
    for (const View& p : payloads) {
        const auto      n = static_cast<std::uint32_t>(p.size());
        const std::byte len[4]{
            static_cast<std::byte>(n & 0xFFu),
            static_cast<std::byte>((n >> 8) & 0xFFu),
            static_cast<std::byte>((n >> 16) & 0xFFu),
            static_cast<std::byte>((n >> 24) & 0xFFu),
        };
        h = fnvUpdate(h, View(len, 4));
        h = fnvUpdate(h, p);
    }
    return h;
}

// ---------------------------------------------------------------- column streams (encode)

struct Streams {
    Bytes lens, ids, heights, bottoms, light;
};

Status encodeRegion(const FullDataSource& s, const Region& r, std::size_t mapSize, Bytes& payload) {
    Streams       st;
    std::uint64_t total = 0;
    for (int x = r.minX; x < r.maxX; ++x) {
        for (int z = r.minZ; z < r.maxZ; ++z) {
            const std::size_t n = s.column(x, z).size();
            if (n > kMaxColumnLen) return invalid("codec: column longer than 4096 points");
            total += n;
            putVar(st.lens, static_cast<std::uint32_t>(n));
        }
    }
    if (total > kMaxPoints) return invalid("codec: too many data points");

    int prevBottom = 0;
    for (int x = r.minX; x < r.maxX; ++x) {
        for (int z = r.minZ; z < r.maxZ; ++z) {
            for (std::uint64_t d : s.column(x, z)) {
                const int id = fdp::id(d);
                if (d != fdp::EMPTY_DATA_POINT && static_cast<std::size_t>(id) >= mapSize)
                    return invalid("codec: data point id outside the id mapping");
                const int          h        = fdp::height(d);
                const int          bottom   = fdp::minY(d);
                const std::uint8_t bl       = fdp::blockLight(d);
                const std::uint8_t sl       = fdp::skyLight(d);
                const bool         hasLight = (bl | sl) != 0;
                const int          expected = prevBottom - h;
                const bool         disc     = bottom != expected;
                putVar(st.ids, (static_cast<std::uint32_t>(id) << 2) | (hasLight ? 2u : 0u) | (disc ? 1u : 0u));
                putVar(st.heights, static_cast<std::uint32_t>(h));
                if (disc) putVar(st.bottoms, varint::zigzag(bottom - expected));
                if (hasLight) st.light.push_back(static_cast<std::byte>((bl << 4) | sl));
                prevBottom = bottom;
            }
        }
    }

    payload.clear();
    for (const Bytes* b : {&st.lens, &st.ids, &st.heights, &st.bottoms, &st.light}) {
        putVar(payload, static_cast<std::uint32_t>(b->size()));
        payload.insert(payload.end(), b->begin(), b->end());
    }
    return Status::ok();
}

// ---------------------------------------------------------------- column streams (decode)

std::optional<View> takeStream(View payload, std::size_t& off) {
    auto len = varint::readVarint(payload, off);
    if (!len || *len > payload.size() - off) return std::nullopt;
    View v  = payload.subspan(off, *len);
    off    += *len;
    return v;
}

Status decodeRegion(View payload, const Region& r, std::size_t mapSize, FullDataSource& out) {
    std::size_t off = 0;
    View        sv[5];
    for (View& v : sv) {
        auto t = takeStream(payload, off);
        if (!t) return corrupt("codec: bad stream length");
        v = *t;
    }
    if (off != payload.size()) return corrupt("codec: trailing bytes after streams");

    // 1. column lengths
    const int                  cols = r.columnCount();
    std::vector<std::uint32_t> lens(static_cast<std::size_t>(cols));
    std::uint64_t              total = 0;
    {
        std::size_t o = 0;
        for (auto& l : lens) {
            auto v = varint::readVarint(sv[0], o);
            if (!v || *v > kMaxColumnLen) return corrupt("codec: bad column length");
            l      = *v;
            total += l;
        }
        if (o != sv[0].size()) return corrupt("codec: trailing bytes in length stream");
    }
    if (total > kMaxPoints || total > sv[1].size()) return corrupt("codec: point count exceeds stream size");

    // 2./3. ids+flags, heights
    std::vector<std::uint32_t> idf(static_cast<std::size_t>(total));
    std::vector<std::uint32_t> heights(static_cast<std::size_t>(total));
    {
        std::size_t o = 0;
        for (auto& v : idf) {
            auto t = varint::readVarint(sv[1], o);
            if (!t) return corrupt("codec: truncated id stream");
            v = *t;
        }
        if (o != sv[1].size()) return corrupt("codec: trailing bytes in id stream");
        o = 0;
        for (auto& v : heights) {
            auto t = varint::readVarint(sv[2], o);
            if (!t || *t > 0xFFFu) return corrupt("codec: bad height");
            v = *t;
        }
        if (o != sv[2].size()) return corrupt("codec: trailing bytes in height stream");
    }

    // 4./5. bottoms + light while assembling columns
    std::size_t  ob = 0, ol = 0, pi = 0, ci = 0;
    std::int64_t prevBottom = 0;
    for (int x = r.minX; x < r.maxX; ++x) {
        for (int z = r.minZ; z < r.maxZ; ++z) {
            auto& col = out.column(x, z);
            col.clear();
            col.reserve(lens[ci]);
            for (std::uint32_t k = 0; k < lens[ci]; ++k, ++pi) {
                const std::uint32_t f      = idf[pi];
                const std::int64_t  height = heights[pi];
                std::int64_t        bottom = prevBottom - height;
                if ((f & 1u) != 0) {
                    auto t = varint::readVarint(sv[3], ob);
                    if (!t) return corrupt("codec: truncated bottomY stream");
                    bottom += varint::unzigzag(*t);
                }
                if (bottom < 0 || bottom > 0xFFF) return corrupt("codec: bottomY out of range");
                std::uint8_t bl = 0, sl = 0;
                if ((f & 2u) != 0) {
                    if (ol >= sv[4].size()) return corrupt("codec: truncated light stream");
                    const auto b = static_cast<std::uint8_t>(sv[4][ol++]);
                    bl           = static_cast<std::uint8_t>(b >> 4);
                    sl           = static_cast<std::uint8_t>(b & 0xFu);
                }
                const std::uint64_t d =
                    fdp::encode(static_cast<int>(f >> 2), static_cast<int>(height), static_cast<int>(bottom), bl, sl);
                if (d != fdp::EMPTY_DATA_POINT && static_cast<std::size_t>(f >> 2) >= mapSize)
                    return corrupt("codec: data point id outside the id mapping");
                col.push_back(d);
                prevBottom = bottom;
            }
            ++ci;
        }
    }
    if (ob != sv[3].size()) return corrupt("codec: trailing bytes in bottomY stream");
    if (ol != sv[4].size()) return corrupt("codec: trailing bytes in light stream");
    return Status::ok();
}

// ---------------------------------------------------------------- RLE (genSteps / world compression modes)

Bytes rleEncode(const std::array<std::int8_t, FullDataSource::COLUMN_COUNT>& a) {
    Bytes         runs;
    std::uint32_t count = 0;
    std::size_t   i     = 0;
    while (i < a.size()) {
        std::size_t j = i + 1;
        while (j < a.size() && a[j] == a[i]) ++j;
        putVar(runs, static_cast<std::uint32_t>(j - i));
        runs.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(a[i])));
        ++count;
        i = j;
    }
    Bytes out;
    putVar(out, count);
    out.insert(out.end(), runs.begin(), runs.end());
    return out;
}

Status rleDecode(View p, std::array<std::int8_t, FullDataSource::COLUMN_COUNT>& a) {
    std::size_t off  = 0;
    auto        runs = varint::readVarint(p, off);
    if (!runs || *runs > a.size()) return corrupt("codec: bad RLE run count");
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < *runs; ++i) {
        auto len = varint::readVarint(p, off);
        if (!len || *len == 0 || *len > a.size() - pos || off >= p.size()) return corrupt("codec: bad RLE run");
        const auto v = static_cast<std::int8_t>(static_cast<std::uint8_t>(p[off++]));
        std::fill_n(a.begin() + static_cast<std::ptrdiff_t>(pos), *len, v);
        pos += *len;
    }
    if (pos != a.size() || off != p.size()) return corrupt("codec: RLE does not cover the section");
    return Status::ok();
}

// ---------------------------------------------------------------- mapping

Result<Bytes> encodeMapping(const IdMap& m) {
    if (m.size() > kMaxMapping) return invalid("codec: id mapping too large");
    std::vector<std::string_view>                       table;
    std::unordered_map<std::string_view, std::uint32_t> index;
    auto                                                intern = [&](const std::string& s) {
        auto it = index.find(std::string_view(s));
        if (it != index.end()) return it->second;
        const auto i = static_cast<std::uint32_t>(table.size());
        table.emplace_back(s);
        index.emplace(std::string_view(s), i);
        return i;
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(m.size());
    for (std::size_t id = 0; id < m.size(); ++id) {
        const BlockInfo* b  = m.block(static_cast<int>(id));
        const BiomeInfo* bi = m.biome(static_cast<int>(id));
        if (!b || !bi) return invalid("codec: id mapping contains a null block/biome");
        const std::uint32_t bs = intern(b->serial);
        const std::uint32_t bo = intern(bi->serial);
        pairs.emplace_back(bs, bo);
    }
    Bytes out;
    putVar(out, static_cast<std::uint32_t>(table.size()));
    for (std::string_view s : table) {
        if (s.size() > kCodecMaxBlobBytes) return invalid("codec: serial too long");
        putVar(out, static_cast<std::uint32_t>(s.size()));
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        out.insert(out.end(), p, p + s.size());
    }
    putVar(out, static_cast<std::uint32_t>(pairs.size()));
    for (auto [b, bi] : pairs) {
        putVar(out, b);
        putVar(out, bi);
    }
    return out;
}

Status decodeMapping(View p, IBlockRegistry& reg, IdMap& out) {
    std::size_t off = 0;
    auto        n   = varint::readVarint(p, off);
    if (!n || *n > p.size() - off || *n > 2ull * kMaxMapping) return corrupt("codec: bad mapping string count");
    // Pass 1 (no allocation): skip the strings, read pairCount, reject amplification before building the table.
    const std::size_t tableStart = off;
    for (std::uint32_t i = 0; i < *n; ++i) {
        auto len = varint::readVarint(p, off);
        if (!len || *len > p.size() - off) return corrupt("codec: bad mapping string");
        off += *len;
    }
    auto pc = varint::readVarint(p, off);
    if (!pc || *pc > kMaxMapping || static_cast<std::uint64_t>(*pc) * 2 > p.size() - off)
        return corrupt("codec: bad mapping pair count");
    if (*n > 2ull * *pc) return corrupt("codec: mapping has more strings than its pairs can use");
    const std::size_t pairsStart = off;

    // Pass 2: now bounded by 2 * pairCount <= remaining bytes.
    std::vector<std::string_view> table;
    table.reserve(*n);
    off = tableStart;
    for (std::uint32_t i = 0; i < *n; ++i) {
        auto len = varint::readVarint(p, off);
        table.emplace_back(reinterpret_cast<const char*>(p.data() + off), *len);
        off += *len;
    }
    off = pairsStart;

    std::vector<const BlockInfo*> blockCache(table.size(), nullptr);
    std::vector<const BiomeInfo*> biomeCache(table.size(), nullptr);
    for (std::uint32_t i = 0; i < *pc; ++i) {
        auto bi = varint::readVarint(p, off);
        auto bo = varint::readVarint(p, off);
        if (!bi || !bo || *bi >= table.size() || *bo >= table.size()) return corrupt("codec: bad mapping index");
        if (!blockCache[*bi]) {
            const BlockInfo* b = reg.resolveBlock(table[*bi]);
            if (!b) {
                log::logf(
                    LogLevel::Warn,
                    "codec: unknown block serial '%.*s', using air",
                    static_cast<int>(table[*bi].size()),
                    table[*bi].data()
                );
                b = reg.air();
            }
            blockCache[*bi] = b;
        }
        if (!biomeCache[*bo]) {
            const BiomeInfo* b = reg.resolveBiome(table[*bo]);
            if (!b) {
                log::logf(
                    LogLevel::Warn,
                    "codec: unknown biome serial '%.*s', using default",
                    static_cast<int>(table[*bo].size()),
                    table[*bo].data()
                );
                b = reg.defaultBiome();
            }
            biomeCache[*bo] = b;
        }
        // addAllAllowingDups keeps ids aligned even if two serials resolve to the same pointer.
        IdMap one;
        one.addIfAbsent(biomeCache[*bo], blockCache[*bi]);
        out.addAllAllowingDups(one);
    }
    if (off != p.size()) return corrupt("codec: trailing bytes in mapping");
    return Status::ok();
}

// ---------------------------------------------------------------- decode driver

Result<FullDataSource> decodeImpl(const StoreRecord& rec, IBlockRegistry& reg, std::optional<AdjDirection> adjOnly) {
    FullDataSource out;
    out.pos     = rec.pos;
    out.mapping = IdMap(rec.pos);
    Bytes storage[8];
    View  gs, cm, mp;
    if (auto st = unframe(rec.genSteps, storage[0], gs); !st) return st.error();
    if (auto st = unframe(rec.compressionModes, storage[1], cm); !st) return st.error();
    if (auto st = unframe(rec.mapping, storage[2], mp); !st) return st.error();
    if (auto st = rleDecode(gs, out.columnGenerationStep); !st) return st.error();
    if (auto st = rleDecode(cm, out.columnWorldCompressionMode); !st) return st.error();
    if (auto st = decodeMapping(mp, reg, out.mapping); !st) return st.error();
    const std::size_t mapSize = out.mapping.size();

    if (adjOnly) {
        for (auto& c : out.columns) c.assign(1, fdp::EMPTY_DATA_POINT);
        const int ai = static_cast<int>(*adjOnly);
        View      v;
        if (auto st = unframe(rec.adjacent[static_cast<std::size_t>(ai)], storage[3], v); !st) return st.error();
        if (auto st = decodeRegion(v, stripRegion(*adjOnly), mapSize, out); !st) return st.error();
    } else {
        View data, adj[4];
        if (auto st = unframe(rec.data, storage[3], data); !st) return st.error();
        for (std::size_t i = 0; i < 4; ++i)
            if (auto st = unframe(rec.adjacent[i], storage[4 + i], adj[i]); !st) return st.error();
        const View payloads[8] = {data, adj[0], adj[1], adj[2], adj[3], gs, cm, mp};
        if (checksumOf(payloads) != rec.meta.dataChecksum) return corrupt("codec: data checksum mismatch");
        if (auto st = decodeRegion(data, kInterior, mapSize, out); !st) return st.error();
        for (std::size_t i = 0; i < 4; ++i)
            if (auto st = decodeRegion(adj[i], stripRegion(static_cast<AdjDirection>(i)), mapSize, out); !st)
                return st.error();
    }

    out.isEmpty = true;
    for (const auto& c : out.columns) {
        for (std::uint64_t d : c) {
            if (d != fdp::EMPTY_DATA_POINT) {
                out.isEmpty = false;
                break;
            }
        }
        if (!out.isEmpty) break;
    }
    out.applyToParent      = rec.meta.hasFlag(StoreFlag::ApplyToParent);
    out.applyToChildren    = rec.meta.hasFlag(StoreFlag::ApplyToChildren);
    out.regenerateLeaf     = rec.meta.hasFlag(StoreFlag::Regenerate);
    out.createdUnixMs      = rec.meta.createdMillis;
    out.lastModifiedUnixMs = rec.meta.lastModifiedMillis;
    return out;
}

} // namespace

// ---------------------------------------------------------------- public API

Result<StoreRecord> encode(const FullDataSource& src, const CodecOptions& opt) {
    StoreRecord rec;
    rec.pos                   = src.pos;
    const std::size_t mapSize = src.mapping.size();
    if (mapSize > kMaxMapping) return invalid("codec: id mapping too large");

    Bytes payloads[5];
    if (auto st = encodeRegion(src, kInterior, mapSize, payloads[0]); !st) return st.error();
    for (std::size_t i = 0; i < 4; ++i)
        if (auto st = encodeRegion(src, stripRegion(static_cast<AdjDirection>(i)), mapSize, payloads[1 + i]); !st)
            return st.error();

    Bytes gsRaw = rleEncode(src.columnGenerationStep);
    Bytes cmRaw = rleEncode(src.columnWorldCompressionMode);
    auto  mpRaw = encodeMapping(src.mapping);
    if (!mpRaw) return mpRaw.error();
    const View views[8]   = {payloads[0], payloads[1], payloads[2], payloads[3], payloads[4], gsRaw, cmRaw, *mpRaw};
    rec.meta.dataChecksum = checksumOf(views);

    auto data = frame(payloads[0], opt);
    if (!data) return data.error();
    rec.data = std::move(*data);
    for (std::size_t i = 0; i < 4; ++i) {
        auto a = frame(payloads[1 + i], opt);
        if (!a) return a.error();
        rec.adjacent[i] = std::move(*a);
    }
    auto gs = frame(gsRaw, opt);
    if (!gs) return gs.error();
    rec.genSteps = std::move(*gs);
    auto cm      = frame(cmRaw, opt);
    if (!cm) return cm.error();
    rec.compressionModes = std::move(*cm);
    auto mp              = encodeMapping(src.mapping);
    if (!mp) return mp.error();
    auto mpf = frame(*mp, opt);
    if (!mpf) return mpf.error();
    rec.mapping = std::move(*mpf);

    rec.meta.dataFormatVersion = kCodecFormatVersion;
    rec.meta.compressionMode   = static_cast<std::uint8_t>(opt.compression);
    rec.meta.flags             = 0;
    rec.meta.setFlag(StoreFlag::ApplyToParent, src.applyToParent.value_or(false));
    rec.meta.setFlag(StoreFlag::ApplyToChildren, src.applyToChildren.value_or(false));
    rec.meta.setFlag(StoreFlag::Regenerate, src.regenerateLeaf.value_or(false));
    rec.meta.createdMillis      = src.createdUnixMs;
    rec.meta.lastModifiedMillis = src.lastModifiedUnixMs;
    return rec;
}

Result<FullDataSource> decode(const StoreRecord& rec, IBlockRegistry& registry) {
    return decodeImpl(rec, registry, std::nullopt);
}

Result<FullDataSource> decodeAdjacentOnly(const StoreRecord& rec, Direction dir, IBlockRegistry& registry) {
    const int ci = compassIndex(dir);
    if (ci < 0) return invalid("codec: decodeAdjacentOnly needs a horizontal direction");
    return decodeImpl(rec, registry, static_cast<AdjDirection>(ci));
}

Result<std::array<std::int8_t, FullDataSource::COLUMN_COUNT>> decodeGenSteps(const StoreRecord& rec) {
    Bytes                                                 storage;
    View                                                  gs;
    std::array<std::int8_t, FullDataSource::COLUMN_COUNT> a{};
    if (auto st = unframe(rec.genSteps, storage, gs); !st) return st.error();
    if (auto st = rleDecode(gs, a); !st) return st.error();
    return a;
}

Result<std::uint32_t> computeDataChecksum(const StoreRecord& rec) {
    Bytes storage[8];
    View  v[8];
    if (auto st = unframe(rec.data, storage[0], v[0]); !st) return st.error();
    for (std::size_t i = 0; i < 4; ++i)
        if (auto st = unframe(rec.adjacent[i], storage[1 + i], v[1 + i]); !st) return st.error();
    if (auto st = unframe(rec.genSteps, storage[5], v[5]); !st) return st.error();
    if (auto st = unframe(rec.compressionModes, storage[6], v[6]); !st) return st.error();
    if (auto st = unframe(rec.mapping, storage[7], v[7]); !st) return st.error();
    return checksumOf(v);
}

void writeRecord(ByteWriter& out, const StoreRecord& rec) {
    out.putU64BE(rec.pos);
    out.putU8(rec.meta.dataFormatVersion);
    out.putU8(rec.meta.compressionMode);
    out.putU8(rec.meta.flags);
    out.putU32BE(rec.meta.dataChecksum);
    out.putI64BE(rec.meta.createdMillis);
    out.putI64BE(rec.meta.lastModifiedMillis);
    auto blob = [&](const Bytes& b) {
        out.putU32BE(static_cast<std::uint32_t>(b.size()));
        out.putBytes(b);
    };
    blob(rec.data);
    for (const Bytes& a : rec.adjacent) blob(a);
    blob(rec.genSteps);
    blob(rec.compressionModes);
    blob(rec.mapping);
}

Result<StoreRecord> readRecord(ByteReader& in) {
    StoreRecord rec;
    auto        pos = in.getU64BE();
    auto        ver = in.getU8();
    auto        cmp = in.getU8();
    auto        fl  = in.getU8();
    auto        cs  = in.getU32BE();
    auto        cr  = in.getI64BE();
    auto        lm  = in.getI64BE();
    if (!pos || !ver || !cmp || !fl || !cs || !cr || !lm) return corrupt("codec: truncated record header");
    rec.pos                     = *pos;
    rec.meta.dataFormatVersion  = *ver;
    rec.meta.compressionMode    = *cmp;
    rec.meta.flags              = *fl;
    rec.meta.dataChecksum       = *cs;
    rec.meta.createdMillis      = *cr;
    rec.meta.lastModifiedMillis = *lm;
    auto blob                   = [&](Bytes& b) -> Status {
        auto n = in.getU32BE();
        if (!n || *n > kCodecMaxBlobBytes || *n > in.remaining()) return corrupt("codec: bad wire blob length");
        auto v = in.getBytes(*n);
        if (!v) return corrupt("codec: truncated wire blob");
        b.assign(v->begin(), v->end());
        return Status::ok();
    };
    if (auto st = blob(rec.data); !st) return st.error();
    for (Bytes& a : rec.adjacent)
        if (auto st = blob(a); !st) return st.error();
    if (auto st = blob(rec.genSteps); !st) return st.error();
    if (auto st = blob(rec.compressionModes); !st) return st.error();
    if (auto st = blob(rec.mapping); !st) return st.error();
    return rec;
}

} // namespace dh
