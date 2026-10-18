#include <doctest.h>

#include "dh/storage/MemoryLodStore.h"
#include "store_conformance.h"

#include <memory>

TEST_CASE("MemoryLodStore conformance") {
    std::shared_ptr<dh::MemoryLodStore> backing;
    runStoreConformance([&](bool reopen) -> std::shared_ptr<dh::ILodStore> {
        if (!reopen || !backing) backing = std::make_shared<dh::MemoryLodStore>();
        return backing;
    });
}

TEST_CASE("MemoryLodStore size/clear helpers") {
    dh::MemoryLodStore s;
    CHECK(s.size() == 0);
    dh::StoreRecord r;
    r.pos = 6;
    REQUIRE(s.put(r).has_value());
    CHECK(s.size() == 1);
    s.clear();
    CHECK(s.size() == 0);
}
