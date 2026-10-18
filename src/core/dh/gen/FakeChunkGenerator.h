#pragma once

// IChunkGenerator test double producing TestChunks of a flat world (solid `ground` below `groundY`, air above).
//
// queue() posts one task onto the injected executor (ManualExecutor in tests, so the test decides when a batch
// runs); the task creates the chunks row-major (x outer, z inner), delivers them through onChunk, then done(true).
// Chunks are retained by the generator until it is destroyed (WorldGenerator needs them alive until processed).
// cancelAll() cancels every batch that has not run yet: done(false) fires before cancelAll() returns.

#include "dh/platform/IChunkGenerator.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/testing/TestChunk.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dh {

class FakeChunkGenerator final : public IChunkGenerator {
public:
    struct Call {
        std::int32_t   minChunkX, minChunkZ, widthChunks;
        GenerationMode mode;
        GenerationStep step;
    };

    FakeChunkGenerator(
        IExecutor&       executor,
        const BlockInfo* air,
        const BlockInfo* ground,
        const BiomeInfo* biome,
        std::int32_t     groundY = 64,
        std::int32_t     minY    = -64,
        std::int32_t     maxY    = 320
    )
    : exec_(executor),
      air_(air),
      ground_(ground),
      biome_(biome),
      groundY_(groundY),
      minY_(minY),
      maxY_(maxY) {}

    // --- observations / knobs ---------------------------------------------------------
    std::vector<Call> calls;
    bool              busy        = false;
    bool              failBatch   = false; ///< next batches deliver no chunks and done(false)
    int               cancelCount = 0;

    std::size_t chunksCreated() const {
        std::lock_guard lock(mutex_);
        return chunks_.size();
    }
    std::size_t pendingBatches() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

    // --- IChunkGenerator ----------------------------------------------------------------
    void queue(
        std::int32_t   minChunkX,
        std::int32_t   minChunkZ,
        std::int32_t   widthChunks,
        GenerationMode mode,
        GenerationStep targetStep,
        ChunkCallback  onChunk,
        DoneCallback   done
    ) override {
        calls.push_back(Call{minChunkX, minChunkZ, widthChunks, mode, targetStep});
        auto batch     = std::make_shared<Batch>();
        batch->minX    = minChunkX;
        batch->minZ    = minChunkZ;
        batch->width   = widthChunks;
        batch->onChunk = std::move(onChunk);
        batch->done    = std::move(done);
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(batch);
        }
        if (!exec_.execute([this, batch] { run(batch); })) finishBatch(batch, false);
    }

    bool isBusy() const override { return busy; }

    void cancelAll() override {
        std::vector<std::shared_ptr<Batch>> toCancel;
        {
            std::lock_guard lock(mutex_);
            ++cancelCount;
            toCancel.swap(pending_);
        }
        for (auto& b : toCancel) {
            DoneCallback cb;
            {
                std::lock_guard lock(mutex_);
                if (b->finished) continue;
                b->finished = true;
                cb          = std::move(b->done);
            }
            if (cb) cb(false);
        }
    }

private:
    struct Batch {
        std::int32_t  minX = 0, minZ = 0, width = 0;
        ChunkCallback onChunk;
        DoneCallback  done;
        bool          finished = false;
    };

    void run(const std::shared_ptr<Batch>& batch) {
        {
            std::lock_guard lock(mutex_);
            if (batch->finished) return; // cancelled
        }
        if (failBatch) {
            finishBatch(batch, false);
            return;
        }
        for (std::int32_t x = 0; x < batch->width; ++x) {
            for (std::int32_t z = 0; z < batch->width; ++z) {
                auto chunk = std::make_unique<TestChunk>(batch->minX + x, batch->minZ + z, minY_, maxY_, air_, biome_);
                chunk->fillLayers(minY_, groundY_, ground_);
                TestChunk* raw = chunk.get();
                {
                    std::lock_guard lock(mutex_);
                    chunks_.push_back(std::move(chunk));
                }
                if (batch->onChunk) batch->onChunk(*raw);
            }
        }
        finishBatch(batch, true);
    }

    void finishBatch(const std::shared_ptr<Batch>& batch, bool ok) {
        DoneCallback cb;
        {
            std::lock_guard lock(mutex_);
            if (batch->finished) return;
            batch->finished = true;
            cb              = std::move(batch->done);
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i] == batch) {
                    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (cb) cb(ok);
    }

    IExecutor&       exec_;
    const BlockInfo* air_;
    const BlockInfo* ground_;
    const BiomeInfo* biome_;
    std::int32_t     groundY_, minY_, maxY_;

    mutable std::mutex                      mutex_;
    std::vector<std::shared_ptr<Batch>>     pending_;
    std::vector<std::unique_ptr<TestChunk>> chunks_;
};

} // namespace dh
