#include "dh/gen/LodRequestModule.h"

#include "dh/pos/SectionPos.h"

#include <algorithm>
#include <utility>

namespace dh {

// ------------------------------------------------------------------ LodRequestModule

LodRequestModule::LodRequestModule(
    GeneratedProvider&         provider,
    IWorldGenCompleteListener& listener,
    QueueFactory               factory
)
: provider_(provider),
  listener_(listener),
  factory_(std::move(factory)) {}

LodRequestModule::~LodRequestModule() { close(); }

void LodRequestModule::tick() {
    bool shouldDoWorldGen = listener_.shouldDoWorldGen();
    if (readOnly_ && readOnly_()) shouldDoWorldGen = false; // :95

    const bool running = isWorldGenRunning();
    if (shouldDoWorldGen && !running) {
        startWorldGen();
    } else if (!shouldDoWorldGen && running) {
        stopWorldGen();
    }

    if (isWorldGenRunning()) {
        if (auto target = listener_.targetPosForGeneration()) queue_->startAndSetTargetPos(*target);
    }
}

void LodRequestModule::startWorldGen() {
    if (queue_ != nullptr || !factory_) return; // Java: "Failed to start world gen due to concurrency"
    std::unique_ptr<IRetrievalQueue> q = factory_();
    if (!q) return;
    queue_ = std::move(q);
    provider_.addWorldGenCompleteListener(&listener_);
    provider_.setRetrievalQueue(queue_.get());
}

void LodRequestModule::stopWorldGen() {
    if (queue_ == nullptr) return; // Java: warn "not running"
    provider_.clearRetrievalQueue();
    queue_->shutdown(true); // synchronized shutdown (closeAsync(true).join())
    queue_.reset();
    provider_.removeWorldGenCompleteListener(&listener_);
}

void LodRequestModule::close() { stopWorldGen(); }

// ------------------------------------------------------------------ LodRequestSet

void LodRequestSet::tryQueuePosForRetrieval(GeneratedProvider& provider, std::uint64_t pos) {
    const auto list = provider.positionsToRetrieve(pos, static_cast<std::uint8_t>(sec::detail(pos)));
    if (!list) return;
    std::lock_guard lock(mutex_);
    for (const std::uint64_t p : *list) {
        if (queued_.count(p) == 0) missing_.insert(p);
    }
}

void LodRequestSet::addMissing(std::uint64_t pos) {
    std::lock_guard lock(mutex_);
    if (queued_.count(pos) == 0) missing_.insert(pos);
}

bool LodRequestSet::isMissing(std::uint64_t pos) const {
    std::lock_guard lock(mutex_);
    return missing_.count(pos) != 0;
}

bool LodRequestSet::isQueued(std::uint64_t pos) const {
    std::lock_guard lock(mutex_);
    return queued_.count(pos) != 0;
}

void LodRequestSet::clear() {
    std::lock_guard lock(mutex_);
    missing_.clear();
    queued_.clear();
}

std::size_t LodRequestSet::missingCount() const {
    std::lock_guard lock(mutex_);
    return missing_.size();
}

std::size_t LodRequestSet::queuedCount() const {
    std::lock_guard lock(mutex_);
    return queued_.size();
}

std::vector<std::uint64_t> LodRequestSet::sortedMissing(BlockPos2D target) const {
    std::vector<std::uint64_t> list;
    {
        std::lock_guard lock(mutex_);
        list.assign(missing_.begin(), missing_.end()); // ascending pos
    }
    std::stable_sort(list.begin(), list.end(), [&](std::uint64_t a, std::uint64_t b) {
        return sec::manhattanBlockDistance(a, target) < sec::manhattanBlockDistance(b, target);
    });
    return list;
}

void LodRequestSet::onFinished(GeneratedProvider& provider, std::uint64_t pos, const RetrievalResult& r) {
    {
        std::lock_guard lock(mutex_);
        queued_.erase(pos);
        if (r.kind == RetrievalResult::Kind::Rejected || r.kind == RetrievalResult::Kind::Failed) {
            missing_.insert(pos); // gen task failed, requeue so we can try again in the future
        }
    }
    if (r.kind == RetrievalResult::Kind::Split) {
        sec::forEachChild(pos, [&](std::uint64_t child) { tryQueuePosForRetrieval(provider, child); });
    }
}

std::size_t LodRequestSet::startQueued(
    GeneratedProvider&                        provider,
    BlockPos2D                                target,
    const std::function<bool(std::uint64_t)>& posInRange
) {
    std::size_t queuedNow = 0;
    for (const std::uint64_t pos : sortedMissing(target)) {
        if (!provider.canQueueRetrievalNow()) break;
        if (posInRange && !posInRange(pos)) continue;

        // mark first: an already-finished future runs the callback inline and must find the entry
        {
            std::lock_guard lock(mutex_);
            queued_.insert(pos);
            missing_.erase(pos);
        }
        auto future = provider.queueRetrieval(pos);
        if (!future) {
            std::lock_guard lock(mutex_);
            queued_.erase(pos);
            missing_.insert(pos);
            continue;
        }
        ++queuedNow;
        future->onComplete([this, &provider, pos](const RetrievalResult& r) { onFinished(provider, pos, r); });
    }

    // don't allow regeneration until the next world gen tick
    provider.setCanRegenerate(false);
    return queuedNow;
}

} // namespace dh
