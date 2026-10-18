#include "bedrock_common/LevelDbLodStore.h"

#include "ll/api/data/KeyValueDB.h"

#include <exception>
#include <system_error>

namespace dh::bedrock {

namespace {
Error ioError(const char* what, const char* detail = nullptr) {
    std::string m = what;
    if (detail) {
        m += ": ";
        m += detail;
    }
    return Error(ErrorCode::Io, std::move(m));
}
} // namespace

LeviKeyValue::LeviKeyValue()  = default;
LeviKeyValue::~LeviKeyValue() = default;

Result<std::shared_ptr<LeviKeyValue>> LeviKeyValue::open(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return ioError("LevelDB: cannot create directory", ec.message().c_str());
    try {
        std::shared_ptr<LeviKeyValue> kv(new LeviKeyValue());
        kv->db_ = std::make_unique<ll::data::KeyValueDB>(dir, true, true, 10);
        return kv;
    } catch (const std::exception& e) {
        return ioError("LevelDB: open failed", e.what());
    } catch (...) {
        return ioError("LevelDB: open failed (unknown exception)");
    }
}

Result<std::optional<std::string>> LeviKeyValue::get(std::string_view key) {
    try {
        return db_->get(key); // nullopt = absent (the wrapper cannot report I/O errors)
    } catch (const std::exception& e) {
        return ioError("LevelDB get failed", e.what());
    } catch (...) {
        return ioError("LevelDB get failed");
    }
}

Status LeviKeyValue::iteratePrefix(
    std::string_view                                               prefix,
    const std::function<bool(std::string_view, std::string_view)>& fn
) {
    try {
        for (auto&& [k, v] : db_->iter()) {
            const std::string_view head = k.substr(0, prefix.size());
            if (head < prefix) continue; // before the prefix range
            if (head > prefix) break;    // past it: keys are ordered
            if (!fn(k, v)) break;
        }
        return Status::ok();
    } catch (const std::exception& e) {
        return ioError("LevelDB iterate failed", e.what());
    } catch (...) {
        return ioError("LevelDB iterate failed");
    }
}

Status LeviKeyValue::write(const KvBatch& batch) {
    if (batch.empty()) return Status::ok();
    try {
        ll::data::KeyValueDB::WriteBatch wb;
        for (const auto& op : batch.ops) {
            if (op.value) wb.set(op.key, *op.value);
            else wb.del(op.key);
        }
        if (!db_->write(wb)) return ioError("LevelDB batch write failed");
        return Status::ok();
    } catch (const std::exception& e) {
        return ioError("LevelDB write failed", e.what());
    } catch (...) {
        return ioError("LevelDB write failed");
    }
}

Result<std::unique_ptr<LevelDbLodStore>> LevelDbLodStore::open(const std::filesystem::path& dir) {
    auto kv = LeviKeyValue::open(dir);
    if (!kv) return kv.error();
    std::unique_ptr<LevelDbLodStore> store(new LevelDbLodStore(std::move(*kv)));
    auto                             st = store->load();
    if (!st) return st.error();
    return store;
}

} // namespace dh::bedrock
