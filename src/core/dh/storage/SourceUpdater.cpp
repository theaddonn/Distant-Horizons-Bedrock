#include "dh/storage/SourceUpdater.h"

#include "dh/build/LightingEngine.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/SourceProvider.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace dh {

SourceUpdater::SourceUpdater(SourceProvider& provider, const UpdaterOptions& options)
: provider_(provider),
  options_(options) {}

namespace {
struct InFlight {
    std::atomic<std::size_t>& n;
    explicit InFlight(std::atomic<std::size_t>& c) : n(c) { n.fetch_add(1); }
    ~InFlight() { n.fetch_sub(1); }
};
} // namespace

Result<bool> SourceUpdater::update(std::shared_ptr<const FullDataSource> input) {
    if (!input) return Error(ErrorCode::InvalidArgument, "null input");
    if (shutdown_.load()) return false;
    InFlight inFlight(inFlight_);

    Result<bool> r(false);
    {
        // A lock is necessary to prevent two threads from writing to the same position at once,
        // otherwise only the second update would apply and the LOD ends up with holes (Java :101).
        auto guard = locks_.lock(input->pos);
        r          = updateLocked(*input);
    }
    // Listeners run after the position lock is released, so a listener may call update() for
    // the same position (Java's ReentrantLock allowed that; KeyedLock does not).
    if (r && *r) {
        notifyListeners(input->pos);
        std::vector<std::pair<ListenerId, DeltaListener>> snapshot;
        {
            std::lock_guard lock(listenerMutex_);
            snapshot = deltaListeners_;
        }
        for (auto& l : snapshot)
            if (l.second) l.second(input);
    }
    return r;
}

Result<bool> SourceUpdater::updateLocked(const FullDataSource& input) {
    if (shutdown_.load()) return false;
    InFlight inFlight(inFlight_);

    const std::uint64_t pos = input.pos;

    // Get or create the data source. Always a private copy (see header).
    std::shared_ptr<FullDataSource> recipient = provider_.loadFresh(pos);
    if (!recipient)
        return Error(ErrorCode::Io, "unable to load recipient section"); // Java: null = shutting down / error

    const std::optional<bool> modified = updateFromDataSource(*recipient, input, options_.update);
    if (!modified.has_value()) {
        return Error(ErrorCode::Unsupported, "updateFromDataSource: unsupported detail level combination");
    }
    if (!*modified) return false;

    // Only update sky lighting at the lowest level, everything above gets it via downsampling
    // (FullDataUpdaterV2:148).
    if (sec::detail(pos) == sec::SECTION_BLOCK_DETAIL_LEVEL) {
        bakeDataSourceSkyLight(*recipient, options_.maxSkyLight);
    }

    // Timestamps (Java: stamped by the SQL layer on upsert).
    const std::int64_t now        = provider_.clock().nowMillis();
    recipient->lastModifiedUnixMs = now;
    if (recipient->createdUnixMs == 0) recipient->createdUnixMs = now;

    auto rec = encode(*recipient, options_.codec);
    if (!rec) return rec.error();
    if (Status st = provider_.store().put(*rec); !st) return st.error();

    provider_.publish(recipient);
    return true;
}

void SourceUpdater::notifyListeners(std::uint64_t pos) {
    InFlight inFlight(inFlight_);
    // Registration order; copies, so (un)registering from a listener is safe.
    std::vector<UpdateListener> snapshot;
    {
        std::lock_guard lock(listenerMutex_);
        snapshot.reserve(listeners_.size());
        for (auto& l : listeners_) snapshot.push_back(l.second);
    }
    for (auto& l : snapshot) {
        if (l) l(pos);
    }
}

void SourceUpdater::waitQuiescent() const {
    while (inFlight_.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

std::shared_ptr<UpdateFuture> SourceUpdater::updateAsync(std::shared_ptr<const FullDataSource> input) {
    auto future = std::make_shared<UpdateFuture>();
    if (!input || shutdown_.load() || options_.asyncExecutor == nullptr) {
        // explicit rejection: Ok(false) strictly means "ran, section unchanged" (callers commit chunk hashes on it)
        future->complete(Result<bool>(ErrorCode::Rejected, "updater shut down or no executor"));
        return future;
    }

    pendingAsync_.fetch_add(1);
    const bool accepted = options_.asyncExecutor->execute([this, input = std::move(input), future] {
        Result<bool> r = update(input);
        if (!r) {
            log::logf(LogLevel::Error, "Async update failed: %s", r.error().message.c_str());
        }
        pendingAsync_.fetch_sub(1);
        future->complete(std::move(r));
    });
    if (!accepted) {
        // Executor shut down while we queued (Java: RejectedExecutionException -> completed future; here an explicit
        // Rejected error).
        pendingAsync_.fetch_sub(1);
        future->complete(Result<bool>(ErrorCode::Rejected, "update executor rejected the task"));
    }
    return future;
}

ListenerId SourceUpdater::addListener(UpdateListener listener) {
    std::lock_guard  lock(listenerMutex_);
    const ListenerId id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

ListenerId SourceUpdater::addDeltaListener(DeltaListener listener) {
    std::lock_guard  lock(listenerMutex_);
    const ListenerId id = nextListenerId_++;
    deltaListeners_.emplace_back(id, std::move(listener));
    return id;
}

void SourceUpdater::removeDeltaListener(ListenerId id) {
    std::lock_guard lock(listenerMutex_);
    std::erase_if(deltaListeners_, [id](const auto& p) { return p.first == id; });
}

void SourceUpdater::removeListener(ListenerId id) {
    std::lock_guard lock(listenerMutex_);
    std::erase_if(listeners_, [id](const auto& p) { return p.first == id; });
}

} // namespace dh
