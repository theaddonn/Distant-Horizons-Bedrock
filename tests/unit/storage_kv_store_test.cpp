#include <doctest.h>

#include "dh/storage/KvLodStore.h"
#include "dh/storage/MapKeyValue.h"
#include "store_conformance.h"

#include <memory>

TEST_CASE("KvLodStore<MapKeyValue> conformance") {
    std::shared_ptr<dh::MapKeyValue> backing;
    runStoreConformance([&](bool reopen) -> std::shared_ptr<dh::ILodStore> {
        if (!reopen || !backing) backing = std::make_shared<dh::MapKeyValue>();
        auto store = dh::KvLodStore::open(backing); // reopen = fresh store over the same keys
        REQUIRE(store.has_value());
        return std::shared_ptr<dh::ILodStore>(std::move(*store));
    });
}

TEST_CASE("KvLodStore key encoding sorts by (detail, x, z) and round-trips") {
    using namespace dh;
    CHECK(kvkey::meta(sec::encode(6, -1, 0)) < kvkey::meta(sec::encode(6, 0, 0)));
    CHECK(kvkey::meta(sec::encode(6, 5, -3)) < kvkey::meta(sec::encode(6, 5, 2)));
    CHECK(kvkey::meta(sec::encode(6, 100, 100)) < kvkey::meta(sec::encode(7, -100, -100)));
    const auto p = sec::encode(9, -1234, 5678);
    CHECK(kvkey::posFromMetaKey(kvkey::meta(p)) == p);
    CHECK_FALSE(kvkey::posFromMetaKey(kvkey::data(p)).has_value());
}

TEST_CASE("KvLodStore: corrupt meta is skipped at open and absent afterwards") {
    using namespace dh;
    auto kv = std::make_shared<MapKeyValue>();
    {
        KvBatch b;
        b.set(kvkey::meta(sec::encode(6, 1, 1)), "short");
        REQUIRE(kv->write(b).has_value());
    }
    auto s = KvLodStore::open(kv);
    REQUIRE(s.has_value());
    auto ex = (*s)->exists(sec::encode(6, 1, 1));
    REQUIRE(ex.has_value());
    CHECK_FALSE(*ex);
}
