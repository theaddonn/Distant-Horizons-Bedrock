#pragma once

// Minimal ordered byte-string key-value abstraction under KvLodStore. The
// Bedrock glue implements it over ll::data::KeyValueDB (LevelDB); tests use
// MapKeyValue. Keys sort lexicographically by unsigned byte. Implementations
// must be safe to call from multiple threads, never throw, and make write()
// atomic (all ops of a batch become visible/durable together or not at all).

#include "dh/util/Result.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dh {

/// Ordered list of puts/deletes applied atomically by IKeyValue::write.
struct KvBatch {
    struct Op {
        std::string                key;
        std::optional<std::string> value; ///< nullopt = delete
    };
    std::vector<Op> ops;

    void set(std::string key, std::string value) { ops.push_back({std::move(key), std::move(value)}); }
    void del(std::string key) { ops.push_back({std::move(key), std::nullopt}); }
    bool empty() const { return ops.empty(); }
};

struct IKeyValue {
    virtual ~IKeyValue() = default;

    /// nullopt if the key is absent; error only on I/O failure.
    virtual Result<std::optional<std::string>> get(std::string_view key) = 0;

    /// Calls `fn(key, value)` for every key starting with `prefix`, in ascending
    /// key order, until `fn` returns false. The views are valid only during the call.
    virtual Status
    iteratePrefix(std::string_view prefix, const std::function<bool(std::string_view, std::string_view)>& fn) = 0;

    /// Applies all ops atomically, in order.
    virtual Status write(const KvBatch& batch) = 0;

    /// Pushes pending writes to durable storage where the backend supports it.
    virtual Status flush() { return Status::ok(); }
};

} // namespace dh
