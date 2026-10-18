#pragma once

// IChunkHashStore over a LevelDB database (shared by the server and client drivers).

#include "bedrock_common/LevelDbLodStore.h"

#include "dh/storage/ChunkHashStore.h"

#include <cstdint>
#include <memory>
#include <string>

namespace dh::bedrock {

/// IChunkHashStore over a LevelDB database: key = x,z (8 bytes big-endian), value = hash (4 bytes big-endian).
class KvChunkHashStore final : public dh::IChunkHashStore {
public:
    explicit KvChunkHashStore(std::shared_ptr<LeviKeyValue> kv) : kv_(std::move(kv)) {}

    std::int32_t get(dh::ChunkPos pos) override {
        auto r = kv_->get(key(pos));
        if (!r || !r->has_value() || r->value().size() != 4) return 0;
        const std::string& s = r->value();
        std::uint32_t      v = 0;
        for (char c : s) v = (v << 8) | static_cast<std::uint8_t>(c);
        return static_cast<std::int32_t>(v);
    }

    dh::Status putBatch(std::span<const dh::ChunkHashEntry> entries) override {
        dh::KvBatch batch;
        for (const auto& e : entries) {
            std::string v(4, '\0');
            const auto  u = static_cast<std::uint32_t>(e.hash);
            for (int i = 0; i < 4; ++i) v[static_cast<std::size_t>(i)] = static_cast<char>((u >> (24 - 8 * i)) & 0xFF);
            batch.set(key(e.pos), std::move(v));
        }
        return kv_->write(batch);
    }

private:
    static std::string key(dh::ChunkPos p) {
        std::string k(8, '\0');
        const auto  x = static_cast<std::uint32_t>(p.x), z = static_cast<std::uint32_t>(p.z);
        for (int i = 0; i < 4; ++i) {
            k[static_cast<std::size_t>(i)]     = static_cast<char>((x >> (24 - 8 * i)) & 0xFF);
            k[static_cast<std::size_t>(4 + i)] = static_cast<char>((z >> (24 - 8 * i)) & 0xFF);
        }
        return k;
    }
    std::shared_ptr<LeviKeyValue> kv_;
};

} // namespace dh::bedrock
