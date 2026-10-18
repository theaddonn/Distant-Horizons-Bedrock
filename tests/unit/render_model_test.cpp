// Tests for WP-E0: render data point layout, ColumnRenderSource and render enums.
// The Java repo has no unit tests covering RenderDataPointUtil / ColumnRenderSource /
// ColumnRenderView (grep of core/src/test/java/tests finds none), so there is nothing to
// port 1:1; the cases below are derived directly from the Java source.
#include <doctest.h>

#include <cstdint>
#include <random>
#include <utility>

#include "dh/render/ColumnRenderSource.h"
#include "dh/render/Enums.h"
#include "dh/render/RenderDataPoint.h"

using dh::BlockMaterial;
using namespace dh;

TEST_CASE("rdp: layout of a hand-built datapoint") {
    // alpha 0xA5 -> nibble 0xA; material Water(12)
    const std::uint64_t d = rdp::create(0xA5, 0x12, 0x34, 0x56, 0x123, 0x045, 7, 9, BlockMaterial::Water);
    CHECK(d == 0xCA12'3456'1230'4597ULL);
    CHECK(rdp::yMax(d) == 0x123);
    CHECK(rdp::yMin(d) == 0x045);
    CHECK(rdp::skyLight(d) == 7);
    CHECK(rdp::blockLight(d) == 9);
    CHECK(rdp::alpha(d) == 0xAF);
    CHECK(rdp::argb(d) == 0xAF123456u);
    CHECK(rdp::material(d) == BlockMaterial::Water);
}

TEST_CASE("rdp: pack/unpack fuzz including materials >= 8") {
    std::mt19937_64 rng(1234);
    for (int i = 0; i < 200000; ++i) {
        const int  a   = static_cast<int>(rng() & 0xFF);
        const int  r   = static_cast<int>(rng() & 0xFF);
        const int  g   = static_cast<int>(rng() & 0xFF);
        const int  b   = static_cast<int>(rng() & 0xFF);
        const int  yH  = static_cast<int>(rng() & 0xFFF);
        const int  yL  = static_cast<int>(rng() & 0xFFF);
        const int  sky = static_cast<int>(rng() & 0xF);
        const int  blk = static_cast<int>(rng() & 0xF);
        const auto m   = static_cast<BlockMaterial>(rng() & 0xF);

        const std::uint64_t d = rdp::create(a, r, g, b, yH, yL, sky, blk, m);
        REQUIRE(rdp::alpha(d) == ((a >> 4) << 4 | 0xF));
        REQUIRE(rdp::red(d) == r);
        REQUIRE(rdp::green(d) == g);
        REQUIRE(rdp::blue(d) == b);
        REQUIRE(rdp::yMax(d) == yH);
        REQUIRE(rdp::yMin(d) == yL);
        REQUIRE(rdp::skyLight(d) == sky);
        REQUIRE(rdp::blockLight(d) == blk);
        REQUIRE(rdp::material(d) == m);
        REQUIRE(
            rdp::argb(d)
            == (static_cast<std::uint32_t>(rdp::alpha(d)) << 24 | static_cast<std::uint32_t>(r) << 16
                | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b))
        );
        // the argb overload must agree with the component overload
        REQUIRE(
            rdp::create(
                yH,
                yL,
                (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16)
                    | (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b),
                sky,
                blk,
                m
            )
            == d
        );
    }
}

TEST_CASE("rdp: alpha quantisation") {
    for (int a = 0; a < 256; ++a) {
        const auto d = rdp::create(a, 0, 0, 0, 0, 0, 0, 0, BlockMaterial::Unknown);
        CHECK(rdp::alpha(d) == ((a & 0xF0) | 0xF));
    }
    CHECK(rdp::alpha(0) == 15);
}

TEST_CASE("rdp: Y fields silently truncate to 12 bits (Java behaviour)") {
    static_assert(rdp::MAX_WORLD_Y_SIZE == 6095);
    const auto d = rdp::create(255, 1, 2, 3, 4096 + 5, 8192 + 7, 1, 2, BlockMaterial::Stone);
    CHECK(rdp::yMax(d) == 5);
    CHECK(rdp::yMin(d) == 7);
    // negative values wrap too, and must not bleed into neighbouring fields
    const auto n = rdp::create(255, 1, 2, 3, -1, -2, 0, 0, BlockMaterial::Unknown);
    CHECK(rdp::yMax(n) == 0xFFF);
    CHECK(rdp::yMin(n) == 0xFFE);
    CHECK(rdp::red(n) == 1);
    CHECK(rdp::material(n) == BlockMaterial::Unknown);
}

TEST_CASE("rdp: setYMin only touches the min Y field") {
    const auto d = rdp::create(255, 10, 20, 30, 100, 50, 3, 4, BlockMaterial::Grass);
    const auto e = rdp::setYMin(d, 77);
    CHECK(rdp::yMin(e) == 77);
    CHECK(rdp::yMax(e) == 100);
    CHECK((e & ~rdp::MIN_Y_SHIFTED_MASK) == (d & ~rdp::MIN_Y_SHIFTED_MASK));
    CHECK(rdp::yMin(rdp::setYMin(d, 0x1005)) == 5);
}

TEST_CASE("rdp: hasZeroHeight / existence") {
    CHECK(rdp::hasZeroHeight(rdp::EMPTY_DATA));
    CHECK_FALSE(rdp::doesDataPointExist(rdp::EMPTY_DATA));

    const auto voidPoint = rdp::create(255, 1, 2, 3, 0, 0, 15, 0, BlockMaterial::Air);
    CHECK(rdp::hasZeroHeight(voidPoint));
    CHECK(rdp::isVoid(voidPoint));
    CHECK(rdp::doesDataPointExist(voidPoint));

    CHECK_FALSE(rdp::hasZeroHeight(rdp::create(255, 0, 0, 0, 1, 0, 0, 0, BlockMaterial::Unknown)));
    CHECK_FALSE(rdp::hasZeroHeight(rdp::create(255, 0, 0, 0, 0, 1, 0, 0, BlockMaterial::Unknown)));
    CHECK_FALSE(rdp::hasZeroHeight(rdp::create(255, 0, 0, 0, 0xFFF, 0xFFF, 0, 0, BlockMaterial::Unknown)));
    CHECK((rdp::VOID_SETTER | 0) == 0xFFFFFF00ULL);
}

TEST_CASE("rdp: comparePriority sign behaviour") {
    const auto lowMat  = rdp::create(255, 0, 0, 0, 10, 0, 0, 0, BlockMaterial::Wood);  // bit 63 clear
    const auto highMat = rdp::create(255, 0, 0, 0, 10, 0, 0, 0, BlockMaterial::Water); // bit 63 set
    CHECK(rdp::comparePriority(lowMat, lowMat) == 0);

    // same word except the lowest 3 bits are ignored by the >>7
    CHECK(rdp::comparePriority(0x1000, 0x1007) == 0);
    CHECK(rdp::comparePriority(0x1008, 0x1000) == 1);
    CHECK(rdp::comparePriority(0x1000, 0x1008) == -1);

    // a negative (material >= 8) word compares lower than a positive one in the signed view:
    // (hi>>7) - (lo>>7) is a huge negative long whose low 32 bits are what Java returns.
    const std::int64_t jl = static_cast<std::int64_t>(highMat) >> 7;
    const std::int64_t jr = static_cast<std::int64_t>(lowMat) >> 7;
    CHECK(rdp::comparePriority(highMat, lowMat) == static_cast<int>(jl - jr));
    CHECK(rdp::comparePriority(lowMat, highMat) == static_cast<int>(jr - jl));
    CHECK(static_cast<std::int64_t>(highMat) < 0);

    // int truncation: difference of 2^32 collapses to 0 (a Java quirk)
    CHECK(rdp::comparePriority(std::uint64_t{1} << 35, 0) == 0);
    CHECK(rdp::comparePriority(std::uint64_t{1} << 34, 0) == INT32_MIN);
}

TEST_CASE("VerticalQuality tables match Java") {
    CHECK(maxVerticalData(VerticalQuality::HeightMap, 0) == 1);
    CHECK(maxVerticalData(VerticalQuality::Low, 0) == 4);
    CHECK(maxVerticalData(VerticalQuality::Low, 3) == 3);
    CHECK(maxVerticalData(VerticalQuality::Medium, 2) == 6);
    CHECK(maxVerticalData(VerticalQuality::Medium, 3) == 4);
    CHECK(maxVerticalData(VerticalQuality::High, 0) == 16);
    CHECK(maxVerticalData(VerticalQuality::High, 2) == 12);
    CHECK(maxVerticalData(VerticalQuality::High, 4) == 8);
    CHECK(maxVerticalData(VerticalQuality::VeryHigh, 0) == 32);
    CHECK(maxVerticalData(VerticalQuality::VeryHigh, 1) == 16);
    CHECK(maxVerticalData(VerticalQuality::Extreme, 0) == 64);
    CHECK(maxVerticalData(VerticalQuality::Extreme, 4) == 16);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 0) == 512);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 5) == 32);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 9) == 16);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 10) == 1);
    // clamp(0, detail, len-1)
    CHECK(maxVerticalData(VerticalQuality::PixelArt, -3) == 512);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 11) == 1);
    CHECK(maxVerticalData(VerticalQuality::PixelArt, 1000) == 1);
    CHECK(ColumnRenderSource::maxSlices(VerticalQuality::High, 2) == 12);
    for (const auto& row : VERTICAL_QUALITY_SLICES) {
        CHECK(row.back() == 1);
    }
}

TEST_CASE("HorizontalQuality and resolution tables match Java") {
    CHECK(info(HorizontalQuality::Lowest).quadraticBase == 2.0);
    CHECK(info(HorizontalQuality::Lowest).distanceUnitInBlocks == 4);
    CHECK(info(HorizontalQuality::Low).distanceUnitInBlocks == 8);
    CHECK(info(HorizontalQuality::Medium).distanceUnitInBlocks == 12);
    CHECK(info(HorizontalQuality::High).quadraticBase == static_cast<double>(2.2f));
    CHECK(info(HorizontalQuality::High).distanceUnitInBlocks == 16);
    CHECK(info(HorizontalQuality::Extreme).quadraticBase == static_cast<double>(2.2f));
    CHECK(info(HorizontalQuality::Extreme).distanceUnitInBlocks == 20);

    CHECK(dataPointLengthCount(MaxHorizontalResolution::Block) == 16);
    CHECK(dataPointWidth(MaxHorizontalResolution::Block) == 1);
    CHECK(dataPointWidth(MaxHorizontalResolution::HalfChunk) == 8);
    CHECK(dataPointWidth(MaxHorizontalResolution::Chunk) == 16);
    CHECK(detailLevel(MaxHorizontalResolution::Chunk) == 4);
    // Block max: 5 candidates, coarsest first
    CHECK(detailForDistance(MaxHorizontalResolution::Block, 0, 100) == MaxHorizontalResolution::Chunk);
    CHECK(detailForDistance(MaxHorizontalResolution::Block, 99, 100) == MaxHorizontalResolution::Block);
    CHECK(detailForDistance(MaxHorizontalResolution::Block, 1000, 100) == MaxHorizontalResolution::Block);
    CHECK(detailForDistance(MaxHorizontalResolution::HalfChunk, 99, 100) == MaxHorizontalResolution::HalfChunk);

    CHECK(noCollision(BlocksToAvoid::NonColliding));
    CHECK_FALSE(noCollision(BlocksToAvoid::None));
}

TEST_CASE("BlockMaterial values match EDhApiBlockMaterial") {
    CHECK(static_cast<int>(BlockMaterial::Unknown) == 0);
    CHECK(static_cast<int>(BlockMaterial::Leaves) == 1);
    CHECK(static_cast<int>(BlockMaterial::Stone) == 2);
    CHECK(static_cast<int>(BlockMaterial::Wood) == 3);
    CHECK(static_cast<int>(BlockMaterial::Metal) == 4);
    CHECK(static_cast<int>(BlockMaterial::Dirt) == 5);
    CHECK(static_cast<int>(BlockMaterial::Lava) == 6);
    CHECK(static_cast<int>(BlockMaterial::DeepSlate) == 7);
    CHECK(static_cast<int>(BlockMaterial::Snow) == 8);
    CHECK(static_cast<int>(BlockMaterial::Sand) == 9);
    CHECK(static_cast<int>(BlockMaterial::Terracotta) == 10);
    CHECK(static_cast<int>(BlockMaterial::NetherStone) == 11);
    CHECK(static_cast<int>(BlockMaterial::Water) == 12);
    CHECK(static_cast<int>(BlockMaterial::Grass) == 13);
    CHECK(static_cast<int>(BlockMaterial::Air) == 14);
    CHECK(static_cast<int>(BlockMaterial::Illuminated) == 15);
}

TEST_CASE("ColumnRenderSource: layout and column spans") {
    auto s = ColumnRenderSource::createEmpty(42, 3, -64);
    CHECK(s.pos == 42);
    CHECK(s.yOffset == -64);
    CHECK(s.isEmpty);
    CHECK(s.data.size() == 64u * 64u * 3u);
    CHECK(s.voidData.size() == 64u * 64u);

    auto col = s.column(5, 7);
    REQUIRE(col.size() == 3);
    col[1] = 0xABCD;
    CHECK(s.data[(5 * 64 * 3) + (7 * 3) + 1] == 0xABCD);
    CHECK(s.dataPoint(5, 7, 1) == 0xABCD);
    CHECK(std::as_const(s).column(5, 7)[1] == 0xABCD);
    CHECK(s.column(64, 0).empty());
    CHECK(s.column(0, -1).empty());

    auto empty = ColumnRenderSource::createEmpty(0, 0, 0);
    CHECK(empty.data.empty());
    CHECK(empty.column(0, 0).empty());
    ColumnRenderView v;
    CHECK_FALSE(empty.populateColumnView(v, 0, 0));
}

TEST_CASE("ColumnRenderSource: populateColumnView / hasNonVoidDataPoints") {
    auto             s = ColumnRenderSource::createEmpty(1, 4, 0);
    ColumnRenderView v;
    CHECK_FALSE(s.populateColumnView(v, -1, 0));
    CHECK_FALSE(s.populateColumnView(v, 0, 64));
    REQUIRE(s.populateColumnView(v, 3, 9));
    CHECK(v.size == 4);
    CHECK(v.maxVerticalSliceCount == 4);
    CHECK(v.dataOffset == 3 * 64 * 4 + 9 * 4);
    CHECK(v.voidDataOffset == 3 * 64 + 9);
    v.set(2, rdp::create(255, 1, 1, 1, 10, 5, 0, 0, BlockMaterial::Stone));
    v.setVoid(7);
    CHECK(s.dataPoint(3, 9, 2) == v.get(2));
    CHECK(s.voidData[3 * 64 + 9] == 7);
    CHECK(v.getVoid() == 7);

    CHECK_FALSE(s.hasNonVoidDataPoints()); // still marked empty
    s.markNotEmpty();
    CHECK(s.hasNonVoidDataPoints());
    v.set(2, rdp::create(255, 1, 1, 1, 0, 0, 0, 0, BlockMaterial::Air));
    CHECK_FALSE(s.hasNonVoidDataPoints());
}

TEST_CASE("VerticalQuality: full table equals Java literal (77 values)") {
    constexpr int expected[7][11] = {
        {1,   1,   1,   1,  1,  1,  1,  1,  1,  1,  1},
        {4,   4,   4,   3,  3,  3,  3,  3,  3,  3,  1},
        {6,   6,   6,   4,  4,  4,  4,  4,  4,  4,  1},
        {16,  16,  12,  12, 8,  8,  8,  8,  8,  8,  1},
        {32,  16,  12,  12, 12, 12, 12, 12, 12, 12, 1},
        {64,  32,  32,  32, 16, 16, 16, 16, 16, 16, 1},
        {512, 256, 128, 64, 32, 32, 16, 16, 16, 16, 1},
    };
    for (int q = 0; q < 7; ++q) {
        for (int d = 0; d < 11; ++d) {
            CHECK(maxVerticalData(static_cast<VerticalQuality>(q), d) == expected[q][d]);
        }
    }
}

TEST_CASE("enum ordinals match Java declaration order") {
    CHECK(static_cast<int>(VerticalQuality::HeightMap) == 0);
    CHECK(static_cast<int>(VerticalQuality::Low) == 1);
    CHECK(static_cast<int>(VerticalQuality::Medium) == 2);
    CHECK(static_cast<int>(VerticalQuality::High) == 3);
    CHECK(static_cast<int>(VerticalQuality::VeryHigh) == 4);
    CHECK(static_cast<int>(VerticalQuality::Extreme) == 5);
    CHECK(static_cast<int>(VerticalQuality::PixelArt) == 6);
    CHECK(static_cast<int>(HorizontalQuality::Lowest) == 0);
    CHECK(static_cast<int>(HorizontalQuality::Low) == 1);
    CHECK(static_cast<int>(HorizontalQuality::Medium) == 2);
    CHECK(static_cast<int>(HorizontalQuality::High) == 3);
    CHECK(static_cast<int>(HorizontalQuality::Extreme) == 4);
    CHECK(static_cast<int>(MaxHorizontalResolution::Block) == 0);
    CHECK(static_cast<int>(MaxHorizontalResolution::TwoBlocks) == 1);
    CHECK(static_cast<int>(MaxHorizontalResolution::FourBlocks) == 2);
    CHECK(static_cast<int>(MaxHorizontalResolution::HalfChunk) == 3);
    CHECK(static_cast<int>(MaxHorizontalResolution::Chunk) == 4);
    CHECK(static_cast<int>(Transparency::Disabled) == 0);
    CHECK(static_cast<int>(Transparency::Complete) == 1);
    CHECK(static_cast<int>(GrassSideRendering::AsGrass) == 0);
    CHECK(static_cast<int>(GrassSideRendering::FadeToDirt) == 1);
    CHECK(static_cast<int>(GrassSideRendering::AsDirt) == 2);
    CHECK(static_cast<int>(LodShading::Auto) == 0);
    CHECK(static_cast<int>(LodShading::Enabled) == 1);
    CHECK(static_cast<int>(LodShading::Disabled) == 2);
    CHECK(static_cast<int>(BlocksToAvoid::None) == 0);
    CHECK(static_cast<int>(BlocksToAvoid::NonColliding) == 1);
}

TEST_CASE("ColumnRenderView: subView, copyFrom and changeVerticalSizeFrom") {
    constexpr std::uint64_t SENTINEL = 0xDEADBEEFULL;

    auto src = ColumnRenderSource::createEmpty(1, 2, 0);
    auto dst = ColumnRenderSource::createEmpty(1, 4, 0);
    std::fill(dst.data.begin(), dst.data.end(), SENTINEL);

    ColumnRenderView sv;
    ColumnRenderView dv;
    REQUIRE(src.populateColumnView(sv, 0, 0));
    REQUIRE(dst.populateColumnView(dv, 0, 0));
    CHECK(sv.subViewCount() == 1);

    sv.set(0, 11);
    sv.set(1, 22);
    sv.setVoid(99);

    CHECK(dv.changeVerticalSizeFrom(sv));
    CHECK(dv.get(0) == 11);
    CHECK(dv.get(1) == 22);
    CHECK(dv.get(2) == 0); // zero padded over the sentinel
    CHECK(dv.get(3) == 0);
    CHECK(dv.getVoid() == 99);
    // neighbouring column untouched
    CHECK(dst.dataPoint(0, 1, 0) == SENTINEL);

    // equal size copy over sentinel
    auto same = ColumnRenderSource::createEmpty(1, 2, 0);
    std::fill(same.data.begin(), same.data.end(), SENTINEL);
    ColumnRenderView samev;
    REQUIRE(same.populateColumnView(samev, 0, 0));
    CHECK(samev.changeVerticalSizeFrom(sv));
    CHECK(samev.get(0) == 11);
    CHECK(samev.get(1) == 22);

    // multi-column: 2 source columns -> 2 destination columns
    ColumnRenderView swide;
    ColumnRenderView dwide;
    REQUIRE(src.populateColumnView(swide, 0, 0));
    REQUIRE(dst.populateColumnView(dwide, 0, 0));
    swide.size = 2 * 2;
    dwide.size = 2 * 4;
    CHECK(swide.subViewCount() == 2);
    CHECK(dwide.subViewCount() == 2);
    swide.set(2, 33);
    swide.set(3, 44);
    CHECK(dwide.changeVerticalSizeFrom(swide));
    CHECK(dwide.get(0) == 11);
    CHECK(dwide.get(1) == 22);
    CHECK(dwide.get(2) == 0);
    CHECK(dwide.get(4) == 33);
    CHECK(dwide.get(5) == 44);
    CHECK(dwide.get(6) == 0);
    CHECK(dwide.get(7) == 0);

    auto second = swide.subView(1, 1);
    CHECK(second.dataOffset == swide.dataOffset + 2);
    CHECK(second.voidDataOffset == swide.voidDataOffset + 1);
    CHECK(second.size == 2);

    // non-zero outputDataIndexOffset: 1 source column into column 1 of a 2-column destination
    std::fill(dst.data.begin(), dst.data.end(), SENTINEL);
    REQUIRE(dst.populateColumnView(dwide, 0, 0));
    dwide.size = 8;
    CHECK(dwide.copyFrom(sv, 1));
    CHECK(dwide.get(0) == SENTINEL);
    CHECK(dwide.get(3) == SENTINEL);
    CHECK(dwide.get(4) == 11);
    CHECK(dwide.get(5) == 22);
    CHECK(dwide.get(6) == 0);
    CHECK(dwide.get(7) == 0);
    CHECK(dwide.get(8) == SENTINEL);
    // offset that does not fit
    CHECK_FALSE(dwide.copyFrom(sv, 2));

    // mismatched column counts are rejected
    CHECK_FALSE(dv.changeVerticalSizeFrom(swide));

    // source wider than destination: copyFrom rejects, changeVerticalSizeFrom is the WP-E1 stub
    CHECK_FALSE(sv.copyFrom(dv, 0));
    CHECK_FALSE(sv.changeVerticalSizeFrom(dv));

    // subView past the end yields an empty view
    auto bad = sv.subView(1 << 20, 1);
    CHECK(bad.size == 0);
    CHECK(bad.data.empty());
}
