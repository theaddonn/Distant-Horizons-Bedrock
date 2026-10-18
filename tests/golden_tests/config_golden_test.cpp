// Golden vectors for the API enums (tests/golden/config.gv.json): one case per Java enum.
//
// SKIPPED_KEYS (no C++ counterpart exists in src/core):
//   EDhApiMaxHorizontalResolution: constants[].startX/startZ/endX/endZ, methods.getSelfAndLowerDetails_*
//   EDhApiBlockMaterial:           methods.getFromIndex_-1_to_17 for indices outside 0..15 (Java -> UNKNOWN;
//                                  C++ has no lookup function)
//   EDhApiDistantGeneratorMode, EDhApiGeneratorPlan, EDhApiDataCompressionMode: whole enums (not ported)
//   EDhApiWorldGenerationStep:     constants[].name (the Java instance field shadows the constant name
//                                  with a lowercase string; ordinals are checked instead)
#include <doctest.h>

#include <bit>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "dh/fulldata/Enums.h"
#include "dh/platform/BlockInfo.h"
#include "dh/render/Enums.h"
#include "golden_loader.h"

using namespace dh;
using namespace dh::test;
using nlohmann::json;

namespace {

long g_checked = 0, g_skipped = 0;

#define CFG_EQ(expected_json, actual)                                                                                  \
    do {                                                                                                               \
        ++g_checked;                                                                                                   \
        CHECK_EQ((expected_json).get<std::int64_t>(), static_cast<std::int64_t>(actual));                              \
    } while (0)

const json& constants(const GoldenCase& c) { return c.out.at("constants"); }

void checkCountAndOrdinals(const GoldenCase& c, std::size_t cppCount) {
    INFO("enum ", c.name);
    ++g_checked;
    CHECK_EQ(c.out.at("count").get<std::size_t>(), cppCount);
    const json& cs = constants(c);
    ++g_checked;
    CHECK_EQ(cs.size(), cppCount);
    for (std::size_t i = 0; i < cs.size(); ++i) {
        INFO("enum ", c.name, " constant ", cs[i].at("name").get<std::string>());
        CFG_EQ(cs[i].at("ordinal"), i);
    }
}

} // namespace

TEST_CASE("golden config: API enums") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "config");
    g_checked = g_skipped = 0;
    std::map<std::string, const GoldenCase*> byEnum;
    for (const GoldenCase& c : s.cases) byEnum[c.in.at("enum").get<std::string>()] = &c;

    SUBCASE("EDhApiVerticalQuality") {
        const GoldenCase& c = *byEnum.at("EDhApiVerticalQuality");
        checkCountAndOrdinals(c, 7);
        const char* names[] = {"HEIGHT_MAP", "LOW", "MEDIUM", "HIGH", "VERY_HIGH", "EXTREME", "PIXEL_ART"};
        for (int q = 0; q < 7; ++q) {
            const json& k = constants(c)[q];
            INFO("constant ", std::string(names[q]));
            CHECK_EQ(k.at("name").get<std::string>(), names[q]);
            const auto  vq  = static_cast<VerticalQuality>(q);
            const json& mvd = k.at("maxVerticalData");
            REQUIRE_EQ(mvd.size(), static_cast<std::size_t>(VERTICAL_QUALITY_DETAIL_COUNT));
            for (int d = 0; d < VERTICAL_QUALITY_DETAIL_COUNT; ++d) {
                INFO("maxVerticalData[", d, "]");
                CFG_EQ(mvd[d], VERTICAL_QUALITY_SLICES[q][d]);
            }
            const std::string mk =
                std::string("calculateMaxNumberOfVerticalSlicesAtDetailLevel_") + names[q] + "_detail_-2_to_14";
            const json& arr = c.out.at("methods").at(mk);
            REQUIRE_EQ(arr.size(), 17u);
            for (int d = -2; d <= 14; ++d) {
                INFO(mk, " detail ", d);
                CFG_EQ(arr[d + 2], maxVerticalData(vq, d));
            }
        }
    }

    SUBCASE("EDhApiHorizontalQuality") {
        const GoldenCase& c = *byEnum.at("EDhApiHorizontalQuality");
        checkCountAndOrdinals(c, 5);
        const char* names[] = {"LOWEST", "LOW", "MEDIUM", "HIGH", "EXTREME"};
        for (int q = 0; q < 5; ++q) {
            const json& k = constants(c)[q];
            INFO("constant ", std::string(names[q]));
            CHECK_EQ(k.at("name").get<std::string>(), names[q]);
            const HorizontalQualityInfo& i = info(static_cast<HorizontalQuality>(q));
            ++g_checked;
            CHECK_EQ(hexU64(k.at("quadraticBase_bits")), std::bit_cast<std::uint64_t>(i.quadraticBase));
            CFG_EQ(k.at("distanceUnitInBlocks"), i.distanceUnitInBlocks);
        }
    }

    SUBCASE("EDhApiMaxHorizontalResolution") {
        const GoldenCase& c = *byEnum.at("EDhApiMaxHorizontalResolution");
        checkCountAndOrdinals(c, 5);
        const char* names[] = {"BLOCK", "TWO_BLOCKS", "FOUR_BLOCKS", "HALF_CHUNK", "CHUNK"};
        for (int r = 0; r < 5; ++r) {
            const json& k = constants(c)[r];
            INFO("constant ", std::string(names[r]));
            CHECK_EQ(k.at("name").get<std::string>(), names[r]);
            const auto res = static_cast<MaxHorizontalResolution>(r);
            CFG_EQ(k.at("dataPointLengthCount"), dataPointLengthCount(res));
            CFG_EQ(k.at("dataPointWidth"), dataPointWidth(res));
            CFG_EQ(k.at("detailLevel"), detailLevel(res));
            g_skipped += 4 + 1; // startX/startZ/endX/endZ + getSelfAndLowerDetails_<R>
        }
    }

    SUBCASE("EDhApiWorldGenerationStep") {
        const GoldenCase& c = *byEnum.at("EDhApiWorldGenerationStep");
        const struct {
            const char*    javaName;
            GenerationStep step;
        } table[] = {
            {"DOWN_SAMPLED",        GenerationStep::DownSampled       },
            {"EMPTY",               GenerationStep::Empty             },
            {"STRUCTURE_START",     GenerationStep::StructureStart    },
            {"STRUCTURE_REFERENCE", GenerationStep::StructureReference},
            {"BIOMES",              GenerationStep::Biomes            },
            {"NOISE",               GenerationStep::Noise             },
            {"SURFACE",             GenerationStep::Surface           },
            {"CARVERS",             GenerationStep::Carvers           },
            {"LIQUID_CARVERS",      GenerationStep::LiquidCarvers     },
            {"TERRAIN",             GenerationStep::Terrain           },
            {"FEATURES",            GenerationStep::Features          },
            {"LIGHT",               GenerationStep::Light             },
        };
        checkCountAndOrdinals(c, std::size(table));
        for (std::size_t i = 0; i < std::size(table); ++i) {
            INFO("constant ", std::string(table[i].javaName));
            CFG_EQ(constants(c)[i].at("value"), static_cast<int>(table[i].step));
            g_skipped += 1; // name
        }
        const json& arr = c.out.at("methods").at("fromValue_-3_to_11");
        REQUIRE_EQ(arr.size(), 15u);
        for (int v = -3; v <= 11; ++v) {
            INFO("fromValue(", v, ")");
            const json& e = arr[v + 3];
            const auto  r = generationStepFromValue(v);
            ++g_checked;
            if (e.is_null()) {
                CHECK_FALSE(r.has_value());
            } else {
                REQUIRE(r.has_value());
                bool found = false;
                for (const auto& t : table) {
                    if (e.get<std::string>() == t.javaName) {
                        found = true;
                        CHECK_EQ(static_cast<int>(*r), static_cast<int>(t.step));
                        break;
                    }
                }
                CHECK(found);
            }
        }
    }

    SUBCASE("EDhApiBlockMaterial") {
        const GoldenCase& c = *byEnum.at("EDhApiBlockMaterial");
        checkCountAndOrdinals(c, 16);
        const struct {
            const char*   javaName;
            BlockMaterial m;
        } table[] = {
            {"UNKNOWN",      BlockMaterial::Unknown    },
            {"LEAVES",       BlockMaterial::Leaves     },
            {"STONE",        BlockMaterial::Stone      },
            {"WOOD",         BlockMaterial::Wood       },
            {"METAL",        BlockMaterial::Metal      },
            {"DIRT",         BlockMaterial::Dirt       },
            {"LAVA",         BlockMaterial::Lava       },
            {"DEEPSLATE",    BlockMaterial::DeepSlate  },
            {"SNOW",         BlockMaterial::Snow       },
            {"SAND",         BlockMaterial::Sand       },
            {"TERRACOTTA",   BlockMaterial::Terracotta },
            {"NETHER_STONE", BlockMaterial::NetherStone},
            {"WATER",        BlockMaterial::Water      },
            {"GRASS",        BlockMaterial::Grass      },
            {"AIR",          BlockMaterial::Air        },
            {"ILLUMINATED",  BlockMaterial::Illuminated},
        };
        for (std::size_t i = 0; i < std::size(table); ++i) {
            const json& k = constants(c)[i];
            INFO("constant ", std::string(table[i].javaName));
            CHECK_EQ(k.at("name").get<std::string>(), table[i].javaName);
            CFG_EQ(k.at("index"), static_cast<int>(table[i].m));
        }
        const json& arr = c.out.at("methods").at("getFromIndex_-1_to_17");
        REQUIRE_EQ(arr.size(), 19u);
        for (int idx = -1; idx <= 17; ++idx) {
            INFO("getFromIndex(", idx, ")");
            if (idx < 0 || idx > 15) {
                ++g_skipped;
                continue;
            }
            ++g_checked;
            CHECK_EQ(arr[idx + 1].get<std::string>(), std::string(table[idx].javaName));
        }
    }

    SUBCASE("EDhApiWorldCompressionMode") {
        const GoldenCase& c = *byEnum.at("EDhApiWorldCompressionMode");
        checkCountAndOrdinals(c, 2);
        const char*                names[] = {"MERGE_SAME_BLOCKS", "VISUALLY_EQUAL"};
        const WorldCompressionMode modes[] = {
            WorldCompressionMode::MergeSameBlocks,
            WorldCompressionMode::VisuallyEqual
        };
        for (int i = 0; i < 2; ++i) {
            INFO("constant ", std::string(names[i]));
            CHECK_EQ(constants(c)[i].at("name").get<std::string>(), names[i]);
            CFG_EQ(constants(c)[i].at("value"), static_cast<int>(modes[i]));
        }
        const json& arr = c.out.at("methods").at("getFromValue_-1_to_2");
        REQUIRE_EQ(arr.size(), 4u);
        for (int v = -1; v <= 2; ++v) {
            INFO("getFromValue(", v, ")");
            const json& e = arr[v + 1];
            const auto  r = compressionModeFromValue(v);
            ++g_checked;
            if (e.is_object() && e.contains("throws")) {
                CHECK_FALSE(r.has_value());
            } else {
                REQUIRE(r.has_value());
                CHECK_EQ(e.get<std::string>(), std::string(names[static_cast<int>(*r)]));
            }
        }
    }

    SUBCASE("not ported: DistantGeneratorMode, GeneratorPlan, DataCompressionMode") {
        for (const char* n : {"EDhApiDistantGeneratorMode", "EDhApiGeneratorPlan", "EDhApiDataCompressionMode"}) {
            REQUIRE(byEnum.count(n) == 1);
            ++g_skipped;
        }
    }

    std::fprintf(
        stderr,
        "[golden] config    cases=%zu keys_checked=%ld keys_skipped=%ld (per-subcase counts)\n",
        s.cases.size(),
        g_checked,
        g_skipped
    );
}
