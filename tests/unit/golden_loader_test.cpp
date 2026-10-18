#include <doctest.h>

#include "golden_loader.h"

using namespace dh::test;

TEST_CASE("golden loader parses the example fixture") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "_example");
    CHECK(s.suite == "_example");
    CHECK(s.javaCommit == "0000000");
    REQUIRE(s.cases.size() == 1);
    const GoldenCase& c = s.cases[0];
    CHECK(c.name == "basic");
    CHECK(hexU64(c.in["a"]) == 0x1a2bULL);
    CHECK(c.in["n"].get<int>() == -5);
    CHECK(hexI64(c.out["neg"]) == -1);
    auto arr = hexU64Array(c.out["arr"]);
    REQUIRE(arr.size() == 2);
    CHECK(arr[0] == 0);
    CHECK(arr[1] == 0xdeadbeefcafebabeULL);
}

TEST_CASE("golden loader returns nullopt for a missing suite") {
    CHECK_FALSE(loadGolden("does_not_exist").has_value());
}

TEST_CASE("golden skip macro skips missing suites") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "does_not_exist");
    FAIL("should have returned early");
}
