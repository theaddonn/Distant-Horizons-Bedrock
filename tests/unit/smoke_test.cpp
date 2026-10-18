#include <doctest.h>

#include "dh/Version.h"

TEST_CASE("dh_core links and reports a version") {
    CHECK(dh::coreVersion()[0] != '\0');
}
