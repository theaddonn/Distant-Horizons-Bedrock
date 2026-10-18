#include "dh/gen/GeneratedProvider.h"

#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"

#include <algorithm>
#include <utility>

namespace dh {

namespace {
constexpr std::int8_t kEmpty       = 0;
constexpr std::int8_t kDownSampled = -1;
constexpr std::int8_t kSurface     = 5;
constexpr std::int8_t kFeatures    = 8;
constexpr std::int8_t kLight       = 9;
} // namespace

GeneratedProvider::GeneratedProvider(
    ILodStore&                      store,
    IBlockRegistry&                 registry,
    IClock&                         clock,
    IExecutor&                      fileHandler,
    const CodecOptions&             codec,
    const ProviderOptions&          options,
    const GeneratedProviderOptions& genOption
)
: SourceProvider(store, registry, clock, fileHandler, codec, options),
  plan_(static_cast<std::uint8_t>(genOption.plan)),
  threads_(genOption.numberOfThreads) {}

// ------------------------------------------------------------------ pure rules

std::int8_t GeneratedProvider::requiredGenerationStep(GeneratorPlan plan, std::int8_t sectionDetail) {
    if (plan == GeneratorPlan::SurfaceOnly) return kSurface;                                         // :370-373
    if (surfaceGenEnabled(plan) && sectionDetail > sec::SECTION_BLOCK_DETAIL_LEVEL) return kSurface; // :374-378
    return kFeatures;                                                                                // :379-382
}

bool GeneratedProvider::isFullyGenerated(std::span<const std::int8_t> steps, std::int8_t requiredStep) {
    for (const std::int8_t s : steps) {
        // :401-403 - NOTE `<=` here
        if (s == kEmpty || s == kDownSampled || s <= requiredStep) return false;
    }
    return true;
}

bool GeneratedProvider::generationStepsAreFullyGenerated(std::span<const std::int8_t> steps) {
    for (const std::int8_t s : steps) {
        if (s == kEmpty || s == kDownSampled) return false;
    }
    return true;
}

bool GeneratedProvider::childNeedsGeneration(std::span<const std::int8_t> steps, std::int8_t requiredStep) {
    std::int8_t currentMin = kLight; // :433
    for (const std::int8_t s : steps) {
        // EDhApiWorldGenerationStep.fromValue: values outside -1..9 have no step and are ignored (:454-461)
        if (s < currentMin && s >= kDownSampled && s <= kLight) currentMin = s;
        // :463-465 - NOTE `<` here
        if (currentMin == kEmpty || currentMin == kDownSampled || currentMin < requiredStep) break;
    }
    // :474-480
    return !(currentMin != kEmpty && currentMin != kDownSampled && currentMin >= requiredStep);
}

// ------------------------------------------------------------------ queue handling

void GeneratedProvider::setQueuedChunkUpdateCounter(std::function<int()> f) {
    std::lock_guard lock(mutex_);
    queuedChunkUpdates_ = std::move(f);
}

bool GeneratedProvider::setRetrievalQueue(IRetrievalQueue* queue) {
    std::lock_guard lock(mutex_);
    if (queue_ != nullptr) return false; // Java asserts "previous world gen queue is still here!"
    queue_ = queue;
    return queue != nullptr;
}

void GeneratedProvider::clearRetrievalQueue() {
    std::lock_guard lock(mutex_);
    queue_ = nullptr;
}

bool GeneratedProvider::hasRetrievalQueue() const {
    std::lock_guard lock(mutex_);
    return queue_ != nullptr;
}

std::int8_t GeneratedProvider::lowestDataDetailLevel() const {
    if (!hasRetrievalQueue()) return sec::SECTION_MINIMUM_DETAIL_LEVEL;
    return static_cast<std::int8_t>(sec::SECTION_MINIMUM_DETAIL_LEVEL + queueLowest_.load());
}

bool GeneratedProvider::canQueueRetrievalNow(bool prune) {
    if (isClosed()) return false;
    IRetrievalQueue*     queue;
    std::function<int()> updates;
    {
        std::lock_guard lock(mutex_);
        queue   = queue_;
        updates = queuedChunkUpdates_;
    }
    if (queue == nullptr) return false;

    const int maxCount     = maxRetrievalQueueCount();
    const int currentCount = updates ? updates() : 0;
    if (currentCount >= maxCount) return false; // :269-272

    const int slots = maxCount - static_cast<int>(queue->waitingTaskCount());
    if (slots == 0) return false;
    if (slots < 0) {
        if (!prune) return false;
        int toCancel = -slots;
        queue->removeRetrievalRequestIf([&toCancel](std::uint64_t) { return toCancel-- > 0; });
    }
    return true;
}

std::shared_ptr<RetrievalFuture> GeneratedProvider::queueRetrieval(std::uint64_t pos) {
    std::shared_ptr<RetrievalFuture> future;
    bool                             created = false;
    {
        std::lock_guard lock(mutex_);
        if (queue_ == nullptr) return nullptr;
        auto it = queued_.find(pos);
        if (it != queued_.end()) {
            future = it->second;
        } else {
            future =
                queue_->submit(pos, static_cast<std::uint8_t>(sec::detail(pos) - sec::SECTION_MINIMUM_DETAIL_LEVEL));
            queued_.emplace(pos, future);
            created = true;
        }
    }
    // attached outside the lock: an already-completed future runs the callback inline
    if (created) future->onComplete([this, pos](const RetrievalResult& r) { onRetrievalResult(pos, r); });
    return future;
}

void GeneratedProvider::onRetrievalResult(std::uint64_t pos, const RetrievalResult& result) {
    if (result.kind == RetrievalResult::Kind::Success && !result.source) {
        // Java LodUtil.assertTrue(:146)
        log::logf(LogLevel::Error, "Successful retrieval object should have a datasource.");
    } else if (result.kind == RetrievalResult::Kind::Success) {
        // shared with the (const) result; the updater only reads it
        std::shared_ptr<const FullDataSource> src = result.source;
        auto r = updater().update(std::move(src)); // Java dataUpdater.updateDataSource (:148)
        if (!r) {
            log::logf(LogLevel::Error, "generated section update failed: %s", r.error().message.c_str());
        } else {
            std::vector<IWorldGenCompleteListener*> copy;
            {
                std::lock_guard lock(listenerMutex_);
                copy = listeners_;
            }
            for (IWorldGenCompleteListener* l : copy) l->onWorldGenTaskComplete(result.pos);
        }
    } else if (result.kind == RetrievalResult::Kind::Failed) {
        log::logf(
            LogLevel::Error,
            "Uncaught Gen Task Exception at [%d,%d,%d], error: [%s].",
            static_cast<int>(sec::detail(pos)),
            sec::x(pos),
            sec::z(pos),
            result.error.c_str()
        );
    }
    // Split (:162-166) and Rejected (shutdown, not logged :136): nothing to do
    std::lock_guard lock(mutex_);
    queued_.erase(pos); // :179
}

void GeneratedProvider::setCanRegenerate(bool v) {
    canRegenerate_.store(v);
    IRetrievalQueue* queue;
    {
        std::lock_guard lock(mutex_);
        queue = queue_;
    }
    if (queue != nullptr) queue->setCanRegenerate(v);
}

void GeneratedProvider::removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) {
    IRetrievalQueue* queue;
    {
        std::lock_guard lock(mutex_);
        queue = queue_;
    }
    if (queue != nullptr) queue->removeRetrievalRequestIf(std::move(removeIf));
}

std::size_t GeneratedProvider::queuedRetrievalCount() const {
    std::lock_guard lock(mutex_);
    return queued_.size();
}

void GeneratedProvider::addWorldGenCompleteListener(IWorldGenCompleteListener* l) {
    std::lock_guard lock(listenerMutex_);
    listeners_.push_back(l);
}

void GeneratedProvider::removeWorldGenCompleteListener(IWorldGenCompleteListener* l) {
    std::lock_guard lock(listenerMutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

// ------------------------------------------------------------------ positionsToRetrieve

std::optional<std::vector<std::int8_t>> GeneratedProvider::loadSteps(std::uint64_t pos) {
    auto rec = store().get(pos, StorePart::GenSteps);
    if (!rec || !rec->has_value()) return std::nullopt; // missing (or unreadable -> regenerate)
    if ((*rec)->genSteps.empty()) return std::vector<std::int8_t>{};
    auto steps = decodeGenSteps(**rec);
    if (!steps) return std::nullopt;
    return std::vector<std::int8_t>(steps->begin(), steps->end());
}

std::optional<std::vector<std::uint64_t>>
GeneratedProvider::positionsToRetrieve(std::uint64_t pos, std::uint8_t generatorDetailLevel) {
    if (!hasRetrievalQueue()) return std::nullopt; // :361-365

    const std::int8_t required = requiredGenerationStep(generatorPlan(), sec::detail(pos));

    // don't check any child positions if this position is already fully generated (:386-416)
    if (auto steps = loadSteps(pos); steps && !steps->empty()) {
        if (isFullyGenerated(*steps, required)) return std::vector<std::uint64_t>{};
    }

    std::vector<std::uint64_t> out;
    sec::forEachChildAtDetailLevel(pos, static_cast<std::int8_t>(generatorDetailLevel), [&](std::uint64_t genPos) {
        auto steps = loadSteps(genPos);
        if (!steps) {
            out.push_back(genPos); // nothing exists for this position (:425-429)
            return;
        }
        if (steps->empty()) return; // "shouldn't happen" (:438-442)
        if (childNeedsGeneration(*steps, required)) out.push_back(genPos);
    });
    return out;
}

} // namespace dh
