#pragma once

// Test doubles for the end-to-end (IT3) world/level scenarios.
//
//   FakeLevelHandle       ILevelHandle over a TestClientLevel (client or server side, optional server link)
//   FakeChunkSource       procedural flat / hilly terrain as TestChunks + a grid that hands out 3x3 neighbours
//   TrackingVertexSink    IVertexSink that validates the 64-byte quad granularity, tracks which sections are
//                         resident, and completes uploads when the test says so
//   CountingTransport     ITransport decorator counting bytes / frames per direction
//   RecordingLogger       ILogger that keeps every message
//   ManualPools           one ManualExecutor per LevelExecutors slot + a "run until idle" pump

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/ITransport.h"
#include "dh/platform/IVertexSink.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/platform/testing/TestClientLevel.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/world/LevelHandle.h"
#include "dh/world/WorldDeps.h"

namespace dh::e2e {

//--------------------------------------------------------------------------------------------------------------
// level handle
//--------------------------------------------------------------------------------------------------------------

class FakeLevelHandle final : public ILevelHandle {
public:
    FakeLevelHandle(std::string id, bool client, std::string dimension = "minecraft:overworld")
    : id_(std::move(id)),
      client_(client) {
        info_.name = std::move(dimension);
    }

    std::string_view  dhIdentifier() const override { return id_; }
    const ILevelInfo& info() const override { return info_; }
    bool              isClientSide() const override { return client_; }
    IClientLevel*     clientLevel() override { return client_ ? &info_ : nullptr; }
    ILevelHandle*     serverSideHandle() override { return serverSide; }
    bool              isServerKeyed() const override { return serverKeyed; }
    void              onUnload() override { ++unloadCalls; }

    TestClientLevel& levelInfo() { return info_; }

    FakeLevelHandle* serverSide  = nullptr; ///< set on client handles of a singleplayer world
    bool             serverKeyed = false;
    int              unloadCalls = 0;

private:
    std::string     id_;
    bool            client_;
    TestClientLevel info_;
};

//--------------------------------------------------------------------------------------------------------------
// terrain
//--------------------------------------------------------------------------------------------------------------

struct Terrain {
    const BlockInfo* grass = nullptr;
    const BlockInfo* dirt  = nullptr;
    const BlockInfo* stone = nullptr;
    const BlockInfo* water = nullptr;
    const BlockInfo* air   = nullptr;
    const BiomeInfo* biome = nullptr;

    static Terrain from(MapBlockRegistry& reg) {
        Terrain t;
        auto    add = [&](const char* serial, BlockMaterial mat, std::uint8_t opacity, bool solid) {
            BlockInfo b;
            b.serial   = serial;
            b.material = mat;
            b.opacity  = opacity;
            b.isSolid  = solid;
            return reg.addBlock(std::move(b));
        };
        t.grass = add("minecraft:grass_block", BlockMaterial::Grass, 16, true);
        t.dirt  = add("minecraft:dirt", BlockMaterial::Dirt, 16, true);
        t.stone = add("minecraft:stone", BlockMaterial::Stone, 16, true);
        t.water = reg.water();
        t.air   = reg.air();
        t.biome = reg.defaultBiome();
        return t;
    }
};

/// Deterministic terrain: Flat is a constant height, Hilly is a sum of two sine ridges (no RNG, no noise library).
class FakeChunkSource {
public:
    enum class Kind { Plain, Flat, Hilly }; // Plain == FakeChunkGenerator's world: stone below y=64, air above

    FakeChunkSource(Terrain terrain, Kind kind, std::int32_t minY = -64, std::int32_t maxY = 320)
    : t_(terrain),
      kind_(kind),
      minY_(minY),
      maxY_(maxY) {}

    static constexpr std::int32_t kSeaLevel = 62;

    std::int32_t heightAt(std::int32_t wx, std::int32_t wz) const {
        if (kind_ != Kind::Hilly) return 64;
        const double h =
            66.0 + 14.0 * std::sin(wx * 0.045) + 9.0 * std::cos(wz * 0.061) + 4.0 * std::sin((wx + wz) * 0.13);
        return static_cast<std::int32_t>(h);
    }

    std::shared_ptr<TestChunk> make(std::int32_t cx, std::int32_t cz) const {
        auto chunk = std::make_shared<TestChunk>(cx, cz, minY_, maxY_, t_.air, t_.biome);
        if (kind_ == Kind::Plain) {
            chunk->fillLayers(minY_, 64, t_.stone);
            return chunk;
        }
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                const std::int32_t h = heightAt(cx * 16 + x, cz * 16 + z);
                chunk->fillColumn(x, z, minY_, h - 3, t_.stone);
                chunk->fillColumn(x, z, h - 3, h, t_.dirt);
                chunk->setBlock(x, h, z, t_.grass);
                if (h < kSeaLevel) chunk->fillColumn(x, z, h + 1, kSeaLevel + 1, t_.water);
            }
        }
        return chunk;
    }

private:
    Terrain      t_;
    Kind         kind_;
    std::int32_t minY_, maxY_;
};

/// Keeps the chunks a test has created so each update can be handed its existing 8 neighbours.
class ChunkGrid {
public:
    explicit ChunkGrid(const FakeChunkSource& source) : source_(source) {}

    std::shared_ptr<TestChunk> get(std::int32_t cx, std::int32_t cz) {
        auto& slot = chunks_[{cx, cz}];
        if (!slot) slot = source_.make(cx, cz);
        return slot;
    }
    std::shared_ptr<TestChunk> find(std::int32_t cx, std::int32_t cz) const {
        auto it = chunks_.find({cx, cz});
        return it == chunks_.end() ? nullptr : it->second;
    }
    std::vector<std::shared_ptr<IChunk>> neighbours(std::int32_t cx, std::int32_t cz) const {
        std::vector<std::shared_ptr<IChunk>> out;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) continue;
                if (auto c = find(cx + dx, cz + dz)) out.push_back(c);
            }
        }
        return out;
    }
    std::size_t size() const { return chunks_.size(); }

private:
    const FakeChunkSource&                                                      source_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::shared_ptr<TestChunk>> chunks_;
};

//--------------------------------------------------------------------------------------------------------------
// vertex sink
//--------------------------------------------------------------------------------------------------------------

class TrackingVertexSink final : public IVertexSink {
public:
    struct Resident {
        std::size_t   opaqueBytes = 0, transparentBytes = 0;
        std::uint64_t uploads = 0;
    };

    void upload(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) override {
        std::lock_guard lock(mutex_);
        auto            check = [&](const std::vector<std::vector<std::byte>>& list) {
            for (const auto& b : list) {
                if (b.size() % kQuadBytes != 0 || b.size() > kMaxBufferBytes) ++violations_;
            }
        };
        check(buffers.opaque);
        check(buffers.transparent);
        totalUploads_++;
        totalBytes_ += buffers.totalBytes();
        Resident& r  = resident_[sectionPos];
        r.uploads++;
        r.opaqueBytes = r.transparentBytes = 0;
        for (const auto& b : buffers.opaque) r.opaqueBytes += b.size();
        for (const auto& b : buffers.transparent) r.transparentBytes += b.size();
        pending_.push_back({sectionPos, std::move(onDone), ++generation_[sectionPos]});
    }

    void remove(std::uint64_t sectionPos) override {
        std::lock_guard lock(mutex_);
        resident_.erase(sectionPos);
        ++generation_[sectionPos]; // pending uploads of this section complete with false
        ++removes_;
    }

    /// Answers every pending upload (true unless the section was removed meanwhile). Returns how many.
    std::size_t completeAll() {
        std::size_t n = 0;
        for (;;) {
            Pending p;
            bool    ok;
            {
                std::lock_guard lock(mutex_);
                if (pending_.empty()) break;
                p = std::move(pending_.front());
                pending_.erase(pending_.begin());
                ok = generation_[p.pos] == p.generation && resident_.count(p.pos) != 0;
            }
            if (p.cb) p.cb(ok);
            ++n;
        }
        return n;
    }

    bool isResident(std::uint64_t pos) const {
        std::lock_guard lock(mutex_);
        return resident_.count(pos) != 0;
    }
    std::size_t residentCount() const {
        std::lock_guard lock(mutex_);
        return resident_.size();
    }
    std::size_t pendingCount() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }
    std::map<std::uint64_t, Resident> residentSnapshot() const {
        std::lock_guard lock(mutex_);
        return resident_;
    }
    std::uint64_t totalUploads() const {
        std::lock_guard lock(mutex_);
        return totalUploads_;
    }
    std::uint64_t totalBytes() const {
        std::lock_guard lock(mutex_);
        return totalBytes_;
    }
    std::uint64_t removes() const {
        std::lock_guard lock(mutex_);
        return removes_;
    }
    /// Buffers that were not whole quads (must stay 0).
    std::uint64_t violations() const {
        std::lock_guard lock(mutex_);
        return violations_;
    }

private:
    struct Pending {
        std::uint64_t  pos = 0;
        UploadCallback cb;
        std::uint64_t  generation = 0;
    };
    mutable std::mutex                     mutex_;
    std::map<std::uint64_t, Resident>      resident_;
    std::map<std::uint64_t, std::uint64_t> generation_;
    std::vector<Pending>                   pending_;
    std::uint64_t                          totalUploads_ = 0, totalBytes_ = 0, removes_ = 0, violations_ = 0;
};

//--------------------------------------------------------------------------------------------------------------
// transport / logger
//--------------------------------------------------------------------------------------------------------------

class CountingTransport final : public ITransport {
public:
    explicit CountingTransport(ITransport& inner) : inner_(inner) {}

    bool send(SessionId session, std::span<const std::byte> frame) override {
        const bool ok = inner_.send(session, frame);
        if (ok) {
            bytes_ += frame.size();
            ++frames_;
            if (frame.size() > maxFrame_) maxFrame_ = frame.size();
            // frame inspection: u16 version | u16 message id | body; FullDataSplit ends with its isFirst byte
            FrameRec rec;
            rec.size = frame.size();
            if (frame.size() >= 4) {
                rec.id = static_cast<std::uint16_t>((static_cast<unsigned>(frame[2]) << 8) | static_cast<unsigned>(frame[3]));
            }
            if (rec.id == kSplitId) {
                rec.first = frame.back() != std::byte{0};
                splitBytes_ += frame.size();
            } else {
                rec.frame.assign(frame.begin(), frame.end());
            }
            log_.push_back(std::move(rec));
        } else {
            ++rejected_;
        }
        return ok;
    }
    std::size_t maxFrameBytes() const override { return inner_.maxFrameBytes(); }
    void        setListener(ITransportListener* l) override { inner_.setListener(l); }
    void        disconnect(SessionId s) override { inner_.disconnect(s); }

    /// One accepted frame: message id peeked from the envelope; the bytes are kept for everything but FullDataSplit.
    struct FrameRec {
        std::uint16_t          id    = 0;
        std::size_t            size  = 0;
        bool                   first = false; ///< FullDataSplit isFirst
        std::vector<std::byte> frame;
    };
    static constexpr std::uint16_t kSplitId = 10; // net::MsgIds::FullDataSplit
    const std::vector<FrameRec>&   log() const { return log_; }
    std::size_t                    countId(std::uint16_t id) const {
        std::size_t n = 0;
        for (const auto& r : log_) n += r.id == id ? 1 : 0;
        return n;
    }
    std::uint64_t splitBytes() const { return splitBytes_; }

    std::uint64_t bytes() const { return bytes_; }
    std::uint64_t frames() const { return frames_; }
    std::uint64_t rejected() const { return rejected_; }
    std::size_t   largestFrame() const { return maxFrame_; }

private:
    ITransport&   inner_;
    std::uint64_t bytes_ = 0, frames_ = 0, rejected_ = 0;
    std::size_t   maxFrame_ = 0;
    std::uint64_t splitBytes_ = 0;
    std::vector<FrameRec> log_;
};

class RecordingLogger final : public ILogger {
public:
    struct Entry {
        LogLevel    level;
        std::string text;
    };
    void log(LogLevel level, std::string_view message) override {
        std::lock_guard lock(mutex_);
        entries.push_back({level, std::string(message)});
    }
    std::size_t count(LogLevel level) const {
        std::lock_guard lock(mutex_);
        std::size_t     n = 0;
        for (const auto& e : entries) n += e.level == level ? 1 : 0;
        return n;
    }
    std::vector<Entry> entries;

private:
    mutable std::mutex mutex_;
};

//--------------------------------------------------------------------------------------------------------------
// executors
//--------------------------------------------------------------------------------------------------------------

/// One deterministic executor per LevelExecutors slot. pump() runs them to idleness in a fixed order.
struct ManualPools {
    ManualExecutor file, lodBuilder, propagator, renderLoader, worldGen, queueThread, network;

    LevelExecutors executors() {
        LevelExecutors e;
        e.fileHandler      = &file;
        e.lodBuilder       = &lodBuilder;
        e.updatePropagator = &propagator;
        e.renderLoader     = &renderLoader;
        e.worldGen         = &worldGen;
        e.queueThread      = &queueThread;
        e.networkWork      = &network;
        return e;
    }
    std::size_t pending() const {
        return file.queueSize() + lodBuilder.queueSize() + propagator.queueSize() + renderLoader.queueSize()
             + worldGen.queueSize() + queueThread.queueSize() + network.queueSize();
    }
    /// Runs every executor until all are empty. Returns the number of tasks run.
    std::size_t runAll(std::size_t maxTasks = 5'000'000) {
        std::size_t total = 0;
        for (;;) {
            std::size_t n  = 0;
            n             += queueThread.runAll();
            n             += worldGen.runAll();
            n             += lodBuilder.runAll();
            n             += file.runAll();
            n             += propagator.runAll();
            n             += renderLoader.runAll();
            n             += network.runAll();
            total         += n;
            if (n == 0 || total > maxTasks) break;
        }
        return total;
    }
};

} // namespace dh::e2e
