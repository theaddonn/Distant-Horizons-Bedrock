// Golden-vector loader for host tests. Header-only.
//
// File format (tests/golden/<suite>.gv.json, produced by tools/golden-dumper):
//   {"suite":"pos","javaCommit":"2206fe7a4","cases":[{"name":"...","in":{...},"out":{...}}]}
// 64-bit values are hex strings "0x1a2b" (negatives as 16 hex digits, two's complement).
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace dh::test {

struct GoldenCase {
    std::string name;
    nlohmann::json in, out;
};

struct GoldenSuite {
    std::string suite, javaCommit;
    std::vector<GoldenCase> cases;
};

// Reads golden/<suiteName>.gv.json relative to the test run directory (tests/).
// Returns nullopt if the file is missing or malformed.
inline std::optional<GoldenSuite> loadGolden(std::string_view suiteName) {
    std::ifstream f(std::string("golden/") + std::string(suiteName) + ".gv.json", std::ios::binary);
    if (!f) return std::nullopt;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("cases") || !j["cases"].is_array()) return std::nullopt;

    GoldenSuite s;
    s.suite = j.value("suite", std::string(suiteName));
    s.javaCommit = j.value("javaCommit", std::string());
    for (const auto& c : j["cases"]) {
        GoldenCase gc;
        gc.name = c.value("name", std::string());
        if (c.contains("in")) gc.in = c["in"];
        if (c.contains("out")) gc.out = c["out"];
        s.cases.push_back(std::move(gc));
    }
    return s;
}

// "0x..." (or bare hex) string -> u64. Throws std::invalid_argument / std::out_of_range on bad input.
inline std::uint64_t hexU64(const nlohmann::json& j) {
    const std::string& s = j.get_ref<const std::string&>();
    std::size_t pos = 0;
    std::uint64_t v = std::stoull(s, &pos, 16);
    if (pos != s.size()) throw std::invalid_argument("bad hex: " + s);
    return v;
}

inline std::int64_t hexI64(const nlohmann::json& j) {
    return static_cast<std::int64_t>(hexU64(j));
}

inline std::vector<std::uint64_t> hexU64Array(const nlohmann::json& j) {
    std::vector<std::uint64_t> out;
    out.reserve(j.size());
    for (const auto& e : j) out.push_back(hexU64(e));
    return out;
}

} // namespace dh::test

// Use at the top of a TEST_CASE. Skips (with a message) when the suite file has not been dumped yet.
#define DH_GOLDEN_SUITE_OR_SKIP(var, name)                                               \
    auto var##_opt = ::dh::test::loadGolden(name);                                       \
    if (!var##_opt) {                                                                    \
        MESSAGE("golden suite '" name "' not found; skipping");                          \
        return;                                                                          \
    }                                                                                    \
    const ::dh::test::GoldenSuite& var = *var##_opt
