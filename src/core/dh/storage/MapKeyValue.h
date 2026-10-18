#pragma once

// std::map-backed IKeyValue: the test backend for KvLodStore (and a reference
// for IKeyValue semantics). Mutex-protected.

#include "dh/storage/IKeyValue.h"

#include <map>
#include <mutex>

namespace dh {

class MapKeyValue final : public IKeyValue {
public:
    Result<std::optional<std::string>> get(std::string_view key) override {
        std::lock_guard lock(mutex_);
        auto            it = map_.find(std::string(key));
        if (it == map_.end()) return std::optional<std::string>{};
        return std::optional<std::string>{it->second};
    }

    Status
    iteratePrefix(std::string_view prefix, const std::function<bool(std::string_view, std::string_view)>& fn) override {
        std::lock_guard lock(mutex_);
        for (auto it = map_.lower_bound(std::string(prefix)); it != map_.end(); ++it) {
            if (std::string_view(it->first).substr(0, prefix.size()) != prefix) break;
            if (!fn(it->first, it->second)) break;
        }
        return Status::ok();
    }

    Status write(const KvBatch& batch) override {
        std::lock_guard lock(mutex_);
        for (const auto& op : batch.ops) {
            if (op.value) map_[op.key] = *op.value;
            else map_.erase(op.key);
        }
        return Status::ok();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return map_.size();
    }

private:
    mutable std::mutex                 mutex_;
    std::map<std::string, std::string> map_;
};

} // namespace dh
