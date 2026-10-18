#pragma once

// Port of generation/DhWorldGenerator.java (the API_DATA_SOURCES generator DH registers itself):
// chunk-generation requests -> IChunkGenerator, generated chunks -> lit -> LodDataBuilder -> one FullDataSource.
//
// Per GenerateRequest (Java generateChunksAsync, :195-270):
//   1. queue widthChunks^2 chunks at the target step of the mode (PreExistingOnly->Empty, Surface->Surface,
//      Features->Features, InternalServer->Light, :201-220)
//   2. the chunks are COLLECTED while the generator runs; when the batch is done they are processed on `genPool`
//      (Java .handle(): also after a failed batch, with whatever chunks arrived - kept)
//   3. each chunk is lit (DhLightingEngine.lightChunk over its 3x3 neighbourhood, AdjacentChunkHolder) and built
//      (LodDataBuilder.createFromChunk); the detail-6 results are merged into a fresh section at
//      request.sectionPos with updateFromDataSource; the section is delivered through `onDone`.
// Requests that chunk generation cannot serve (chunkGenAllowedForDetailLevel false: detail > 0, plan without
// chunk gen, or PreExistingOnly with surface gen) go to the optional rough generator hook; without one they
// fail with ErrorCode::Unsupported (Java threw NullPointerException).
//
// Contract requirement beyond IChunkGenerator.h: the IChunk objects delivered to onChunk must stay valid until
// the batch's `done` has fired and the processing task finished (Java kept the wrappers in a list).
//
// close(): cancels the chunk generator and completes every outstanding request exactly once with
// ErrorCode::Cancelled; later chunk/done callbacks are ignored.
// Deviation: Java's getLargestDataDetailLevel reads the live config; here the plan is a setter.

#include "dh/build/LightingEngine.h"
#include "dh/build/LodDataBuilder.h"
#include "dh/config/Enums.h"
#include "dh/fulldata/Merge.h"
#include "dh/gen/IWorldGenerator.h"
#include "dh/platform/IChunkGenerator.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace dh {

struct DhWorldGeneratorOptions {
    GeneratorPlan plan        = GeneratorPlan::SurfaceThenChunks;
    int           maxSkyLight = 15;
    BuildOptions  build;
    UpdateOptions update;
};

class WorldGenerator final : public IWorldGenerator {
public:
    /// Rough surface estimation for requests the chunk generator cannot serve (Java IRoughGenerator).
    using RoughGenerateFn = std::function<void(const GenerateRequest&, IExecutor& genPool, DoneCallback)>;

    WorldGenerator(IChunkGenerator& chunkGenerator, const DhWorldGeneratorOptions& options = {});
    ~WorldGenerator() override;

    void setGeneratorPlan(GeneratorPlan p) { plan_.store(static_cast<std::uint8_t>(p)); }
    void setRoughGenerator(RoughGenerateFn fn) { rough_ = std::move(fn); }

    /// Java chunkGenAllowedForDetailLevel (:142-170).
    static bool chunkGenAllowed(GeneratorPlan plan, std::int8_t dataDetail, GenerationMode mode);
    /// Java :201-220.
    static GenerationStep targetStepFor(GenerationMode mode);

    // IWorldGenerator
    std::int8_t smallestDataDetailLevel() const override { return 0; }
    std::int8_t largestDataDetailLevel() const override;
    void
    generate(const GenerateRequest& request, IExecutor& genPool, ChunkCallback onChunk, DoneCallback onDone) override;
    bool isBusy() const override { return chunkGen_.isBusy(); }
    void preGeneratorTaskStart() override {}
    void close() override;

    /// Requests started and not yet completed (tests).
    std::size_t inFlight() const;

private:
    struct Job;
    void process(const std::shared_ptr<Job>& job);
    void finish(const std::shared_ptr<Job>& job, Result<std::shared_ptr<FullDataSource>> result);

    IChunkGenerator&          chunkGen_;
    DhWorldGeneratorOptions     options_;
    std::atomic<std::uint8_t> plan_;
    RoughGenerateFn           rough_;

    mutable std::mutex                            mutex_;
    bool                                          closed_ = false;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs_; ///< keyed by a private counter (deterministic order)
    std::uint64_t                                 nextId_ = 1;
};

} // namespace dh
