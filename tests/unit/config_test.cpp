// Tests for WP-I: config options, presets and the JSON store.
// Enum tables are checked against tests/golden/config.gv.json (dumped from the Java enums).
#include <doctest.h>

#include <bit>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dh/config/ConfigStore.h"
#include "dh/config/Options.h"
#include "dh/config/Presets.h"
#include "golden_loader.h"

using namespace dh;

namespace {

const dh::test::GoldenCase* findCase(const dh::test::GoldenSuite& s, const char* name) {
    for (const auto& c : s.cases) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

std::string upper(std::string s) {
    for (auto& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return s;
}

/// Compare golden constant names/ordinals with the C++ option's enum name table.
void checkNames(const nlohmann::json& out, const char* optionPath) {
    const OptionMeta* m = findOptionMeta(optionPath);
    REQUIRE(m != nullptr);
    REQUIRE(m->type == OptionType::Enum);
    CHECK(m->enumCount == out["count"].get<int>());
    const auto& constants = out["constants"];
    REQUIRE(constants.size() == static_cast<std::size_t>(m->enumCount));
    for (const auto& c : constants) {
        const int ordinal = c["ordinal"].get<int>();
        REQUIRE(ordinal >= 0);
        REQUIRE(ordinal < m->enumCount);
        CHECK(std::string(m->enumNames[ordinal]) == c["name"].get<std::string>());
    }
}

} // namespace

//=========================//
// golden-backed enum tables //
//=========================//

TEST_CASE("config golden: VerticalQuality") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* c = findCase(g, "EDhApiVerticalQuality");
    REQUIRE(c != nullptr);
    checkNames(c->out, "client.graphics.quality.verticalQuality");
    for (const auto& k : c->out["constants"]) {
        const auto  q   = static_cast<VerticalQuality>(k["ordinal"].get<int>());
        const auto& arr = k["maxVerticalData"];
        REQUIRE(arr.size() == static_cast<std::size_t>(VERTICAL_QUALITY_DETAIL_COUNT));
        for (int d = 0; d < VERTICAL_QUALITY_DETAIL_COUNT; ++d) {
            CHECK(maxVerticalData(q, d) == arr[static_cast<std::size_t>(d)].get<int>());
        }
    }
}

TEST_CASE("config golden: HorizontalQuality") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* c = findCase(g, "EDhApiHorizontalQuality");
    REQUIRE(c != nullptr);
    checkNames(c->out, "client.graphics.quality.horizontalQuality");
    for (const auto& k : c->out["constants"]) {
        const auto& i = info(static_cast<HorizontalQuality>(k["ordinal"].get<int>()));
        CHECK(std::bit_cast<std::uint64_t>(i.quadraticBase) == dh::test::hexU64(k["quadraticBase_bits"]));
        CHECK(i.distanceUnitInBlocks == k["distanceUnitInBlocks"].get<int>());
    }
}

TEST_CASE("config golden: MaxHorizontalResolution") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* c = findCase(g, "EDhApiMaxHorizontalResolution");
    REQUIRE(c != nullptr);
    checkNames(c->out, "client.graphics.quality.maxHorizontalResolution");
    for (const auto& k : c->out["constants"]) {
        const auto r = static_cast<MaxHorizontalResolution>(k["ordinal"].get<int>());
        CHECK(dataPointLengthCount(r) == k["dataPointLengthCount"].get<int>());
        CHECK(dataPointWidth(r) == k["dataPointWidth"].get<int>());
        CHECK(detailLevel(r) == k["detailLevel"].get<int>());
    }
}

TEST_CASE("config golden: WorldGenerationStep") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* c = findCase(g, "EDhApiWorldGenerationStep");
    REQUIRE(c != nullptr);
    // Java fromValue returns the first constant with a value; C++ has the same first-match rule.
    const auto& vals = c->out["methods"]["fromValue_-3_to_11"];
    for (std::size_t i = 0; i < vals.size(); ++i) {
        const int  v   = static_cast<int>(i) - 3;
        const auto got = generationStepFromValue(v);
        if (vals[i].is_null()) {
            CHECK_FALSE(got.has_value());
        } else {
            REQUIRE(got.has_value());
            CHECK(static_cast<int>(*got) == v);
        }
    }
    for (const auto& k : c->out["constants"]) {
        const int value = k["value"].get<int>();
        CHECK(generationStepFromValue(value).has_value());
    }
}

TEST_CASE("config golden: GeneratorPlan, DistantGeneratorMode") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* plan = findCase(g, "EDhApiGeneratorPlan");
    REQUIRE(plan != nullptr);
    checkNames(plan->out, "common.worldGenerator.generatorPlan");
    for (const auto& k : plan->out["constants"]) {
        const auto p = static_cast<GeneratorPlan>(k["ordinal"].get<int>());
        CHECK(generationEnabled(p) == k["generationEnabled"].get<bool>());
        CHECK(chunkGenEnabled(p) == k["chunkGenEnabled"].get<bool>());
        CHECK(surfaceGenEnabled(p) == k["surfaceGenEnabled"].get<bool>());
    }
    const auto* mode = findCase(g, "EDhApiDistantGeneratorMode");
    REQUIRE(mode != nullptr);
    checkNames(mode->out, "common.worldGenerator.chunkGeneratorMode");
    for (const auto& k : mode->out["constants"]) {
        CHECK(complexity(static_cast<DistantGeneratorMode>(k["ordinal"].get<int>())) == k["complexity"].get<int>());
    }
}

TEST_CASE("config golden: DataCompressionMode, WorldCompressionMode") {
    DH_GOLDEN_SUITE_OR_SKIP(g, "config");
    const auto* dc = findCase(g, "EDhApiDataCompressionMode");
    REQUIRE(dc != nullptr);
    checkNames(dc->out, "common.lodBuilding.dataCompression");
    for (const auto& k : dc->out["constants"]) {
        const auto m = static_cast<DataCompressionMode>(k["ordinal"].get<int>());
        CHECK(dataCompressionValue(m) == k["value"].get<int>());
        CHECK(
            isSupported(m)
            == (k["name"].get<std::string>() == "UNCOMPRESSED" || k["name"].get<std::string>() == "Z_STD_BLOCK")
        );
    }
    const auto* wc = findCase(g, "EDhApiWorldCompressionMode");
    REQUIRE(wc != nullptr);
    checkNames(wc->out, "common.lodBuilding.worldCompression");
    for (const auto& k : wc->out["constants"]) {
        const auto v   = k["value"].get<int>();
        const auto got = compressionModeFromValue(v);
        REQUIRE(got.has_value());
        CHECK(static_cast<int>(*got) == v);
        CHECK(static_cast<int>(*got) == k["ordinal"].get<int>());
    }
}

TEST_CASE("config: names of the remaining enums (Java declaration order)") {
    const auto names = [](const char* path) {
        std::vector<std::string> v;
        const OptionMeta*        m = findOptionMeta(path);
        REQUIRE(m != nullptr);
        for (int i = 0; i < m->enumCount; ++i) v.emplace_back(m->enumNames[i]);
        return v;
    };
    CHECK(names("client.graphics.quality.transparency") == std::vector<std::string>{"DISABLED", "COMPLETE"});
    CHECK(names("client.graphics.quality.lodShading") == std::vector<std::string>{"AUTO", "ENABLED", "DISABLED"});
    CHECK(
        names("client.graphics.quality.grassSideRendering")
        == std::vector<std::string>{"AS_GRASS", "FADE_TO_DIRT", "AS_DIRT"}
    );
    CHECK(names("client.graphics.culling.blocksToIgnore") == std::vector<std::string>{"NONE", "NON_COLLIDING"});
    CHECK(
        names("client.graphics.quality.vanillaFadeMode")
        == std::vector<std::string>{"NONE", "SINGLE_PASS", "DOUBLE_PASS"}
    );
    CHECK(
        names("common.worldGenerator.showGenerationProgress")
        == std::vector<std::string>{"OVERLAY", "CHAT", "LOG", "DISABLED"}
    );
}

//=======//
// table //
//=======//

TEST_CASE("config: metadata table agrees with struct defaults") {
    const Options     d = defaultOptions(4);
    const OptionMeta* t = optionMeta();
    CHECK(optionMetaCount() > 50);
    for (std::size_t i = 0; i < optionMetaCount(); ++i) {
        const OptionMeta& m = t[i];
        CAPTURE(m.path);
        CHECK(m.comment != nullptr);
        // unique path
        for (std::size_t j = i + 1; j < optionMetaCount(); ++j) CHECK(std::strcmp(m.path, t[j].path) != 0);
        const void* p = m.constPtr(d);
        switch (m.type) {
        case OptionType::Bool:
            CHECK(*static_cast<const bool*>(p) == (m.defaultValue != 0.0));
            break;
        case OptionType::Int:
            CHECK(static_cast<double>(*static_cast<const std::int32_t*>(p)) == m.defaultValue);
            break;
        case OptionType::Float:
            CHECK(static_cast<double>(*static_cast<const float*>(p)) == m.defaultValue);
            break;
        case OptionType::Double:
            CHECK(*static_cast<const double*>(p) == m.defaultValue);
            break;
        case OptionType::String:
            CHECK(*static_cast<const std::string*>(p) == m.defaultString);
            break;
        case OptionType::Enum:
            CHECK(static_cast<double>(*static_cast<const std::uint8_t*>(p)) == m.defaultValue);
            CHECK(m.enumCount > 0);
            CHECK(m.maxValue == m.enumCount - 1);
            break;
        }
        if (m.type != OptionType::String && m.type != OptionType::Bool) {
            CHECK(m.minValue <= m.defaultValue);
            CHECK(m.defaultValue <= m.maxValue);
        }
    }
}

TEST_CASE("config: spot-check Java defaults and ranges") {
    const Options o;
    CHECK(o.client.graphics.quality.lodChunkRenderDistanceRadius == 256);
    CHECK(o.client.graphics.quality.brightnessMultiplier == 1.0F);
    CHECK(o.client.graphics.culling.overdrawPrevention == -1.0F);
    CHECK(o.common.worldGenerator.chunkGeneratorMode == DistantGeneratorMode::Features);
    CHECK(o.common.lodBuilding.worldCompression == WorldCompressionMode::VisuallyEqual);
    CHECK(o.server.maxGenerationRequestDistance == 4096);
    const OptionMeta* r = findOptionMeta("client.graphics.quality.lodChunkRenderDistanceRadius");
    REQUIRE(r != nullptr);
    CHECK(r->minValue == 32);
    CHECK(r->maxValue == 4096);
    CHECK(findOptionMeta("nope") == nullptr);
}

//==========//
// clamping //
//==========//

TEST_CASE("config: clampToRange") {
    Options o;
    o.client.graphics.quality.lodChunkRenderDistanceRadius                    = 1;
    o.client.graphics.quality.maxZoomQualityIncrease                          = 99;
    o.client.graphics.quality.lodBiomeBlending                                = -5;
    o.client.graphics.culling.overdrawPrevention                              = 7.0F;
    o.client.graphics.culling.caveCullingHeight                               = -100000;
    o.client.graphics.quality.brightnessMultiplier                            = std::nanf("");
    o.client.graphics.quality.saturationMultiplier                            = 5.0F; // no Java range -> untouched
    o.common.multiThreading.threadRunTimeRatio                                = 0.0;
    o.common.multiThreading.numberOfThreads                                   = 64;
    o.server.playerBandwidthLimit                                             = -3;
    o.server.generationRequestRateLimit                                       = 1000;
    o.common.worldGenerator.generationMaxChunkRadius                          = -1;
    o.common.worldGenerator.surfaceRegenMaxDistancePercent                    = 1e9;
    *reinterpret_cast<std::uint8_t*>(&o.client.graphics.quality.transparency) = 9; // bogus ordinal
    o.common.lodBuilding.dataCompression                                      = DataCompressionMode::Lz4;

    clampToRange(o, 8);
    CHECK(o.client.graphics.quality.lodChunkRenderDistanceRadius == 32);
    CHECK(o.client.graphics.quality.maxZoomQualityIncrease == 6);
    CHECK(o.client.graphics.quality.lodBiomeBlending == 0);
    CHECK(o.client.graphics.culling.overdrawPrevention == 1.0F);
    CHECK(o.client.graphics.culling.caveCullingHeight == -4096);
    CHECK(o.client.graphics.quality.brightnessMultiplier == 1.0F);
    CHECK(o.client.graphics.quality.saturationMultiplier == 5.0F);
    CHECK(o.common.multiThreading.threadRunTimeRatio == 0.01);
    CHECK(o.common.multiThreading.numberOfThreads == 8);
    CHECK(o.server.playerBandwidthLimit == 0);
    CHECK(o.server.generationRequestRateLimit == 100);
    CHECK(o.common.worldGenerator.generationMaxChunkRadius == 0);
    CHECK(o.common.worldGenerator.surfaceRegenMaxDistancePercent == 1000.0);
    CHECK(o.client.graphics.quality.transparency == Transparency::Complete);
    CHECK(o.common.lodBuilding.dataCompression == DataCompressionMode::ZStdBlock);

    o.common.multiThreading.numberOfThreads = 0;
    clampToRange(o, 0);
    CHECK(o.common.multiThreading.numberOfThreads == 1);
    o.common.multiThreading.numberOfThreads = 5000; // unknown core count: no upper bound
    clampToRange(o, 0);
    CHECK(o.common.multiThreading.numberOfThreads == 5000);
}

//=========//
// presets //
//=========//

TEST_CASE("config: quality preset table (Java RenderQualityPresetConfigEventHandler)") {
    Options o;
    applyQualityPreset(o, QualityPreset::Minimum);
    const auto& q = o.client.graphics.quality;
    CHECK(q.maxHorizontalResolution == MaxHorizontalResolution::TwoBlocks);
    CHECK(q.verticalQuality == VerticalQuality::HeightMap);
    CHECK(q.horizontalQuality == HorizontalQuality::Lowest);
    CHECK(q.transparency == Transparency::Disabled);
    CHECK_FALSE(o.client.graphics.enableSsao);
    CHECK_FALSE(o.client.graphics.enableAntiAliasing);
    CHECK(q.vanillaFadeMode == VanillaFadeMode::None);
    CHECK_FALSE(q.ditherDhFade);
    CHECK(o.client.graphics.culling.enableCaveCulling);
    CHECK(q.lodBiomeBlending == 0);

    applyQualityPreset(o, QualityPreset::Low);
    CHECK(q.maxHorizontalResolution == MaxHorizontalResolution::Block);
    CHECK(q.verticalQuality == VerticalQuality::Low);
    CHECK(q.transparency == Transparency::Disabled);
    CHECK(q.vanillaFadeMode == VanillaFadeMode::SinglePass);
    CHECK(q.lodBiomeBlending == 1);

    applyQualityPreset(o, QualityPreset::High);
    CHECK(q.verticalQuality == VerticalQuality::High);
    CHECK_FALSE(o.client.graphics.culling.enableCaveCulling);

    applyQualityPreset(o, QualityPreset::Extreme);
    CHECK(q.verticalQuality == VerticalQuality::Extreme);
    CHECK(q.horizontalQuality == HorizontalQuality::Extreme);

    // the default options are the MEDIUM preset
    CHECK(detectQualityPreset(Options{}) == QualityPreset::Medium);
}

TEST_CASE("config: quality preset round trip and CUSTOM") {
    for (int p = 1; p <= static_cast<int>(QualityPreset::Extreme); ++p) {
        const auto preset = static_cast<QualityPreset>(p);
        Options    o;
        applyQualityPreset(o, preset);
        CHECK(detectQualityPreset(o) == preset);
        // options outside the table do not matter
        o.client.graphics.quality.lodChunkRenderDistanceRadius = 999;
        CHECK(detectQualityPreset(o) == preset);
        // mutating any controlled option -> Custom
        Options m                                  = o;
        m.client.graphics.quality.lodBiomeBlending = (m.client.graphics.quality.lodBiomeBlending == 2) ? 0 : 2;
        CHECK(detectQualityPreset(m) == QualityPreset::Custom);
        m                            = o;
        m.client.graphics.enableSsao = !m.client.graphics.enableSsao;
        CHECK(detectQualityPreset(m) == QualityPreset::Custom);
        m = o;
        m.client.graphics.quality.vanillaFadeMode =
            VanillaFadeMode::SinglePass == o.client.graphics.quality.vanillaFadeMode ? VanillaFadeMode::None
                                                                                     : VanillaFadeMode::SinglePass;
        CHECK(detectQualityPreset(m) == QualityPreset::Custom);
    }
    Options c;
    applyQualityPreset(c, QualityPreset::Custom); // no-op
    CHECK(detectQualityPreset(c) == QualityPreset::Medium);
}

TEST_CASE("config: thread preset tables") {
    // Java: ceil(cores * percent) clamped to [1, cores]
    struct Row {
        int cores, minimal, low, balanced, aggressive, whole;
    };
    const Row rows[] = {
        {4,  1, 1, 2, 3,  4 },
        {8,  1, 2, 4, 6,  8 },
        {16, 2, 4, 8, 12, 16},
        {1,  1, 1, 1, 1,  1 },
        {12, 2, 3, 6, 9,  12},
    };
    for (const Row& r : rows) {
        CAPTURE(r.cores);
        CHECK(threadPresetThreadCount(ThreadPreset::MinimalImpact, r.cores) == r.minimal);
        CHECK(threadPresetThreadCount(ThreadPreset::LowImpact, r.cores) == r.low);
        CHECK(threadPresetThreadCount(ThreadPreset::Balanced, r.cores) == r.balanced);
        CHECK(threadPresetThreadCount(ThreadPreset::Aggressive, r.cores) == r.aggressive);
        CHECK(threadPresetThreadCount(ThreadPreset::IPaidForTheWholeCpu, r.cores) == r.whole);
        CHECK(defaultOptions(r.cores).common.multiThreading.numberOfThreads == r.balanced);
    }
    CHECK(threadPresetRunTimeRatio(ThreadPreset::MinimalImpact) == 0.5);
    CHECK(threadPresetRunTimeRatio(ThreadPreset::LowImpact) == 1.0);
    CHECK(threadPresetRunTimeRatio(ThreadPreset::Balanced) == 1.0);
}

TEST_CASE("config: thread preset round trip and CUSTOM") {
    for (int cores : {4, 8, 16, 32}) {
        for (int p = 1; p <= static_cast<int>(ThreadPreset::IPaidForTheWholeCpu); ++p) {
            const auto preset = static_cast<ThreadPreset>(p);
            Options    o;
            applyThreadPreset(o, preset, cores);
            clampToRange(o, cores);
            CHECK(detectThreadPreset(o, cores) == preset);
            Options m                                  = o;
            m.common.multiThreading.threadRunTimeRatio = 0.77;
            CHECK(detectThreadPreset(m, cores) == ThreadPreset::Custom);
        }
    }
    // with 1 core every count is 1; first matching preset in enum order wins
    Options o;
    applyThreadPreset(o, ThreadPreset::LowImpact, 1);
    CHECK(detectThreadPreset(o, 1) == ThreadPreset::LowImpact);
    Options c                               = defaultOptions(8);
    c.common.multiThreading.numberOfThreads = 5;
    CHECK(detectThreadPreset(c, 8) == ThreadPreset::Custom);
    CHECK(detectThreadPreset(defaultOptions(8), 8) == ThreadPreset::Balanced);
}

//==============//
// ConfigStore  //
//==============//

TEST_CASE("config store: defaults, save/load round trip") {
    ConfigStore a(8);
    CHECK(a.options().common.multiThreading.numberOfThreads == 4);

    Options& o                                             = a.options();
    o.client.graphics.quality.lodChunkRenderDistanceRadius = 512;
    o.client.graphics.quality.verticalQuality              = VerticalQuality::PixelArt;
    o.client.graphics.quality.brightnessMultiplier         = 0.1F;
    o.client.graphics.quality.saturationMultiplier         = 1.3F;
    o.client.graphics.culling.overdrawPrevention           = 0.3F;
    o.client.graphics.culling.ignoredRenderBlockCsv        = "minecraft:stone,\xC3\xA4";
    o.common.worldGenerator.generatorPlan                  = GeneratorPlan::ChunksOnly;
    o.common.worldGenerator.surfaceRegenMaxDistancePercent = 0.123456789012345;
    o.common.worldGenerator.generationCenterChunkX         = INT_MIN;
    o.common.lodBuilding.dataCompression                   = DataCompressionMode::Uncompressed;
    o.common.multiThreading.numberOfThreads                = 7;
    o.server.serverKey                                     = "key \"quoted\"";
    o.server.playerBandwidthLimit                          = 1234;

    const std::string text = a.save();
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"verticalQuality\": \"PIXEL_ART\"") != std::string::npos);
    CHECK(text.find("\"brightnessMultiplier\": 0.1") != std::string::npos); // shortest float text
    CHECK(text.find("0.10000000149") == std::string::npos);

    ConfigStore b(8);
    const auto  err = b.load(text);
    CHECK_FALSE(err.has_value());
    CHECK(b.warnings().empty());
    CHECK(b.save() == text);
    CHECK(b.options().client.graphics.quality.brightnessMultiplier == 0.1F);
    CHECK(b.options().client.graphics.quality.saturationMultiplier == 1.3F);
    CHECK(b.options().common.worldGenerator.surfaceRegenMaxDistancePercent == 0.123456789012345);
    CHECK(b.options().common.worldGenerator.generationCenterChunkX == INT_MIN);
    CHECK(b.options().common.worldGenerator.generatorPlan == GeneratorPlan::ChunksOnly);
    CHECK(b.options().common.lodBuilding.dataCompression == DataCompressionMode::Uncompressed);
    CHECK(b.options().server.serverKey == "key \"quoted\"");
    CHECK(b.options().client.graphics.culling.ignoredRenderBlockCsv == "minecraft:stone,\xC3\xA4");
}

TEST_CASE("config store: default options save/load is the identity") {
    ConfigStore a(4);
    ConfigStore b(4);
    CHECK_FALSE(b.load(a.save()).has_value());
    CHECK(b.save() == a.save());
    CHECK(b.options().client.graphics.quality.horizontalQuality == HorizontalQuality::Medium);
}

TEST_CASE("config store: unknown keys ignored, missing keys defaulted, values clamped") {
    ConfigStore s(8);
    s.options().server.serverKey = "stale"; // load replaces everything
    const char* text             = R"({
        "version": 1,
        "somethingNew": {"a": [1,2,3]},
        "client": {
            "futureSection": 5,
            "graphics": {
                "quality": {
                    "lodChunkRenderDistanceRadius": 100000,
                    "maxZoomQualityIncrease": 2.4,
                    "verticalQuality": "EXTREME",
                    "horizontalQuality": 4,
                    "transparency": "NOPE",
                    "lodBiomeBlending": "three",
                    "brightnessMultiplier": 2
                },
                "enableSsao": false
            }
        },
        "common": { "multiThreading": { "numberOfThreads": 1000, "threadRunTimeRatio": -3 },
                     "lodBuilding": { "dataCompression": "LZ4" } },
        "server": "not an object"
    })";
    const auto  err              = s.load(text);
    CHECK_FALSE(err.has_value());
    const Options& o = s.options();
    CHECK(o.client.graphics.quality.lodChunkRenderDistanceRadius == 4096);
    CHECK(o.client.graphics.quality.maxZoomQualityIncrease == 2);
    CHECK(o.client.graphics.quality.verticalQuality == VerticalQuality::Extreme);
    CHECK(o.client.graphics.quality.horizontalQuality == HorizontalQuality::Extreme);
    CHECK(o.client.graphics.quality.transparency == Transparency::Complete); // bad enum -> default
    CHECK(o.client.graphics.quality.lodBiomeBlending == 3);                  // bad type -> default
    CHECK(o.client.graphics.quality.brightnessMultiplier == 2.0F);
    CHECK_FALSE(o.client.graphics.enableSsao);
    CHECK(o.client.graphics.enableAntiAliasing);         // missing -> default
    CHECK(o.common.multiThreading.numberOfThreads == 8); // clamped to cores
    CHECK(o.common.multiThreading.threadRunTimeRatio == 0.01);
    CHECK(o.common.lodBuilding.dataCompression == DataCompressionMode::ZStdBlock);
    CHECK(o.server.serverKey.empty());
    CHECK(o.server.playerBandwidthLimit == 500);
    CHECK(s.warnings().size() >= 3);
}

TEST_CASE("config store: bad input is an error and leaves options untouched") {
    ConfigStore s(4);
    s.options().server.serverKey = "keep";
    CHECK(s.load("").has_value());
    CHECK(s.load("{not json").has_value());
    CHECK(s.load("[1,2]").has_value());
    CHECK(s.load("42").has_value());
    CHECK(s.options().server.serverKey == "keep");
    // empty object is valid: everything defaults
    CHECK_FALSE(s.load("{}").has_value());
    CHECK(s.options().server.serverKey.empty());
}

TEST_CASE("config store: listeners") {
    ConfigStore s(4);
    int         calls = 0;
    int         last  = 0;
    s.onChange([&](const Options& o) {
        ++calls;
        last = o.client.graphics.quality.lodChunkRenderDistanceRadius;
    });
    int second = 0;
    s.onChange([&](const Options&) { ++second; });
    CHECK(calls == 0);

    s.set([](Options& o) { o.client.graphics.quality.lodChunkRenderDistanceRadius = 128; });
    CHECK(calls == 1);
    CHECK(last == 128);
    CHECK(second == 1);

    s.set([](Options& o) { o.client.graphics.quality.lodChunkRenderDistanceRadius = 5; }); // clamped before notifying
    CHECK(last == 32);

    Options replacement;
    replacement.client.graphics.quality.lodChunkRenderDistanceRadius = 64;
    s.set(replacement);
    CHECK(last == 64);

    s.options().client.graphics.quality.lodChunkRenderDistanceRadius = 96;
    s.notify();
    CHECK(last == 96);
    CHECK(calls == 4);

    CHECK_FALSE(s.load("{}").has_value());
    CHECK(calls == 5);
    CHECK(last == 256);
    CHECK(s.load("garbage").has_value());
    CHECK(calls == 5); // failed load does not notify

    // quality preset applied through the store is observable by listeners
    QualityPreset seen = QualityPreset::Custom;
    s.onChange([&](const Options& o) { seen = detectQualityPreset(o); });
    s.set([](Options& o) { applyQualityPreset(o, QualityPreset::Extreme); });
    CHECK(seen == QualityPreset::Extreme);
}
