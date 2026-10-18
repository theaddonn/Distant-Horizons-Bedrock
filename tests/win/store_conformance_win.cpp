// Runs the ILodStore conformance suite against LevelDbLodStore (LeviLamina's
// KeyValueDB). Windows-only; needs LeviLamina.dll. See docs/STORAGE.md.
#include <doctest.h>

#include "bedrock_common/LevelDbLodStore.h"
#include "store_conformance.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>

TEST_CASE("LevelDbLodStore conformance") {
    static std::atomic<int> counter{0};
    const auto              root = std::filesystem::temp_directory_path()
                    / ("dh_store_conf_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                       + "_" + std::to_string(counter++));
    std::filesystem::path dir;
    runStoreConformance([&](bool reopen) -> std::shared_ptr<dh::ILodStore> {
        if (!reopen || dir.empty()) dir = root / std::to_string(counter++);
        auto store = dh::bedrock::LevelDbLodStore::open(dir);
        if (!store) MESSAGE("open failed: ", store.error().message);
        REQUIRE(store.has_value());
        return std::shared_ptr<dh::ILodStore>(std::move(*store));
    });
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
