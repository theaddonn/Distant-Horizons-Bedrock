#include "dh/gen/WorldGenerator.h"

#include "dh/platform/IChunk.h"
#include "dh/platform/IExecutor.h"

#include <array>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace dh {

struct WorldGenerator::Job {
    std::uint64_t        id = 0;
    GenerateRequest      request;
    ChunkCallback        onChunk;
    DoneCallback         onDone;
    std::vector<IChunk*> chunks;
    bool                 done = false; ///< guarded by WorldGenerator::mutex_
};

namespace {

/// Java AdjacentChunkHolder: the generated chunks addressable by chunk position.
class AdjacentChunkHolder {
public:
    explicit AdjacentChunkHolder(const std::vector<IChunk*>& chunks) {
        for (IChunk* c : chunks) byPos_.emplace(std::make_pair(c->chunkX(), c->chunkZ()), c); // first one wins
    }
    IChunk* at(std::int32_t x, std::int32_t z) const {
        auto it = byPos_.find({x, z});
        return it == byPos_.end() ? nullptr : it->second;
    }

private:
    std::map<std::pair<std::int32_t, std::int32_t>, IChunk*> byPos_;
};

} // namespace

WorldGenerator::WorldGenerator(IChunkGenerator& chunkGenerator, const DhWorldGeneratorOptions& options)
: chunkGen_(chunkGenerator),
  options_(options),
  plan_(static_cast<std::uint8_t>(options.plan)) {}

WorldGenerator::~WorldGenerator() { close(); }

bool WorldGenerator::chunkGenAllowed(GeneratorPlan plan, std::int8_t dataDetail, GenerationMode mode) {
    if (!chunkGenEnabled(plan)) return false; // :147
    if (dataDetail > 0) return false;         // :152 chunks only at detail 0
    if (surfaceGenEnabled(plan) && mode == GenerationMode::PreExistingOnly) return false; // :157
    return true;
}

GenerationStep WorldGenerator::targetStepFor(GenerationMode mode) {
    switch (mode) {
    case GenerationMode::PreExistingOnly:
        return GenerationStep::Empty; // special logic
    case GenerationMode::Surface:
        return GenerationStep::Surface;
    case GenerationMode::Features:
        return GenerationStep::Features;
    case GenerationMode::InternalServer:
        return GenerationStep::Light;
    }
    return GenerationStep::Features;
}

std::int8_t WorldGenerator::largestDataDetailLevel() const {
    // :104-116 any LOD detail when surface gen is enabled, otherwise chunks only
    return surfaceGenEnabled(static_cast<GeneratorPlan>(plan_.load())) ? 12 : 0;
}

std::size_t WorldGenerator::inFlight() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

void WorldGenerator::generate(
    const GenerateRequest& request,
    IExecutor&             genPool,
    ChunkCallback          onChunk,
    DoneCallback           onDone
) {
    auto job     = std::make_shared<Job>();
    job->request = request;
    job->onChunk = std::move(onChunk);
    job->onDone  = std::move(onDone);

    bool closed;
    {
        std::lock_guard lock(mutex_);
        closed = closed_;
        if (!closed) {
            job->id = nextId_++;
            jobs_.emplace(job->id, job);
        }
    }
    if (closed) {
        if (job->onDone) job->onDone(Error{ErrorCode::Cancelled, "generator closed"});
        return;
    }

    const auto plan = static_cast<GeneratorPlan>(plan_.load());
    if (!chunkGenAllowed(plan, request.dataDetail, request.mode)) {
        if (!rough_) {
            finish(job, Error{ErrorCode::Unsupported, "No rough generator provided"});
            return;
        }
        rough_(request, genPool, [this, job](Result<std::shared_ptr<FullDataSource>> r) { finish(job, std::move(r)); });
        return;
    }

    IExecutor* pool = &genPool;
    chunkGen_.queue(
        request.minChunkX,
        request.minChunkZ,
        request.widthChunks,
        request.mode,
        targetStepFor(request.mode),
        [this, job](IChunk& chunk) {
            {
                std::lock_guard lock(mutex_);
                if (job->done) return;
                job->chunks.push_back(&chunk);
            }
            if (job->onChunk) job->onChunk(chunk);
        },
        [this, job, pool](bool /*ok*/) {
            // Java .handle(): process whatever arrived, success or not
            {
                std::lock_guard lock(mutex_);
                if (job->done) return;
            }
            if (!pool->execute([this, job] { process(job); })) {
                finish(job, Error{ErrorCode::Rejected, "gen pool rejected task"});
            }
        }
    );
}

void WorldGenerator::process(const std::shared_ptr<Job>& job) {
    std::vector<IChunk*> chunks;
    {
        std::lock_guard lock(mutex_);
        if (job->done) return; // cancelled by close()
        chunks = job->chunks;
    }

    const AdjacentChunkHolder holder(chunks);
    auto result = std::make_shared<FullDataSource>(FullDataSource::createEmpty(job->request.sectionPos));

    LightingOptions light;
    light.maxSkyLight = options_.maxSkyLight;

    for (IChunk* chunk : chunks) {
        std::array<IChunk*, 8> nearby{};
        std::size_t            n = 0;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) continue;
                if (IChunk* nb = holder.at(chunk->chunkX() + dx, chunk->chunkZ() + dz)) nearby[n++] = nb;
            }
        }
        lightChunk(*chunk, std::span<IChunk* const>(nearby.data(), n), light);

        if (auto built = createFromChunk(*chunk, options_.build)) {
            (void)updateFromDataSource(*result, *built, options_.update);
        }
    }
    finish(job, std::move(result));
}

void WorldGenerator::finish(const std::shared_ptr<Job>& job, Result<std::shared_ptr<FullDataSource>> result) {
    DoneCallback cb;
    {
        std::lock_guard lock(mutex_);
        if (job->done) return;
        job->done = true;
        jobs_.erase(job->id);
        cb = std::move(job->onDone);
    }
    if (cb) cb(std::move(result));
}

void WorldGenerator::close() {
    std::vector<std::shared_ptr<Job>> pending;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        for (auto& [id, job] : jobs_) {
            job->done = true;
            pending.push_back(job);
        }
        jobs_.clear();
    }
    chunkGen_.cancelAll(); // late done(false) callbacks find job->done and return
    for (auto& job : pending) {
        DoneCallback cb = std::move(job->onDone);
        if (cb) cb(Error{ErrorCode::Cancelled, "generator closed"});
    }
}

} // namespace dh
