// Shared helpers for the golden-vector tests. One Ctx per golden case; every eq*() call
// checks one `out` key against the C++ value with an informative failure context.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>

#include "golden_loader.h"

namespace dh::test::gv {

struct Stats {
    long cases = 0, checked = 0, skipped = 0;
};

inline bool isThrows(const nlohmann::json& j) { return j.is_object() && j.contains("throws"); }

class Ctx {
public:
    Ctx(const GoldenCase& c, Stats& s) : c_(c), s_(s) { ++s_.cases; }

    const GoldenCase& gc() const { return c_; }
    bool              has(const char* k) const { return c_.out.contains(k); }
    bool              throws(const char* k) const { return isThrows(c_.out.at(k)); }

    int           inI(const char* k) const { return c_.in.at(k).get<int>(); }
    double        inD(const char* k) const { return c_.in.at(k).get<double>(); }
    std::uint64_t inHex(const char* k) const { return hexU64(c_.in.at(k)); }
    std::uint64_t outHex(const char* k) const { return hexU64(c_.out.at(k)); }

    /// For keys without a C++ rejection variant: false (and counted as skipped) when Java threw,
    /// so the caller must not evaluate the C++ function for that input. Also false when absent.
    bool live(const char* k) {
        if (!has(k)) return false;
        if (throws(k)) {
            ++s_.skipped;
            return false;
        }
        return true;
    }

    /// Check key `k` (a plain value, not a nested object) against `actual`.
    template <class T>
    void eq(const char* k, T actual) {
        eqJson(c_.out.at(k), k, actual);
    }
    /// Check a value nested at out[k][sub].
    template <class T>
    void eqSub(const char* k, const char* sub, T actual) {
        const std::string key = std::string(k) + "." + sub;
        eqJson(c_.out.at(k).at(sub), key.c_str(), actual);
    }
    /// Java threw (out[k] is {"throws"}) <=> C++ rejected the input. For values nested in out[k] use eqSub.
    void eqRejected(const char* k, bool cppRejected) {
        ++s_.checked;
        INFO(
            "case ",
            c_.name,
            " key ",
            std::string(k),
            " (Java ",
            std::string(throws(k) ? "throws" : "returns a value"),
            ")"
        );
        CHECK_EQ(throws(k), cppRejected);
    }
    /// Try/optional variant: Java threw <=> C++ returned nullopt.
    template <class T>
    void eqOpt(const char* k, const std::optional<T>& actual) {
        const nlohmann::json& j = c_.out.at(k);
        if (isThrows(j)) {
            ++s_.checked;
            INFO("case ", c_.name, " key ", std::string(k), " (Java throws ", j["throws"].get<std::string>(), ")");
            CHECK_FALSE(actual.has_value());
            return;
        }
        INFO("case ", c_.name, " key ", std::string(k), " (Java returned a value, C++ returned nullopt)");
        REQUIRE(actual.has_value());
        eqJson(j, k, *actual);
    }

private:
    template <class T>
    void eqJson(const nlohmann::json& j, const char* k, T actual) {
        ++s_.checked;
        INFO("case ", c_.name, " key ", std::string(k));
        if constexpr (std::is_same_v<T, bool>) {
            CHECK_EQ(j.get<bool>(), actual);
        } else if constexpr (std::is_floating_point_v<T>) {
            CHECK_EQ(j.get<double>(), static_cast<double>(actual));
        } else if constexpr (std::is_same_v<T, std::string>) {
            CHECK_EQ(j.get<std::string>(), actual);
        } else {
            if (j.is_string()) { // hex-encoded Java long
                CHECK_EQ(hexU64(j), static_cast<std::uint64_t>(actual));
            } else {
                CHECK_EQ(j.get<std::int64_t>(), static_cast<std::int64_t>(actual));
            }
        }
    }

    const GoldenCase& c_;
    Stats&            s_;
};

inline void report(const char* suite, const Stats& s) {
    std::fprintf(
        stderr,
        "[golden] %-9s cases=%ld keys_checked=%ld keys_skipped=%ld\n",
        suite,
        s.cases,
        s.checked,
        s.skipped
    );
}

} // namespace dh::test::gv
