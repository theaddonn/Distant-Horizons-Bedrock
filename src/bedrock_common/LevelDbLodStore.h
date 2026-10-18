#pragma once

// LevelDB-backed ILodStore (see docs/STORAGE.md). All store logic lives in the
// core's KvLodStore; this file only adapts ll::data::KeyValueDB to IKeyValue.
//
// KeyValueDB API facts this relies on: get / has / set / del / write(WriteBatch)
// (atomic LevelDB WriteBatch) / iter() (ordered full scan; there is no seek, so
// prefix iteration skips ahead through smaller keys and stops at the first key
// past the prefix). Errors surface only as bool/nullopt (no status), and the
// constructor may throw: every call here is wrapped and converted to dh::Error.

#include "dh/storage/KvLodStore.h"

#include <filesystem>
#include <memory>

namespace ll::data {
class KeyValueDB;
}

namespace dh::bedrock {

/// IKeyValue over a LevelDB database opened through LeviLamina.
class LeviKeyValue final : public IKeyValue {
public:
    static Result<std::shared_ptr<LeviKeyValue>> open(const std::filesystem::path& dir);
    ~LeviKeyValue() override;

    Result<std::optional<std::string>> get(std::string_view key) override;
    Status
    iteratePrefix(std::string_view prefix, const std::function<bool(std::string_view, std::string_view)>& fn) override;
    Status write(const KvBatch& batch) override;

private:
    LeviKeyValue();
    std::unique_ptr<ll::data::KeyValueDB> db_;
};

class LevelDbLodStore final : public KvLodStore {
public:
    /// Opens (creating if needed) the database in `dir` and rebuilds the indexes.
    static Result<std::unique_ptr<LevelDbLodStore>> open(const std::filesystem::path& dir);

private:
    explicit LevelDbLodStore(std::shared_ptr<IKeyValue> kv) : KvLodStore(std::move(kv)) {}
};

} // namespace dh::bedrock
