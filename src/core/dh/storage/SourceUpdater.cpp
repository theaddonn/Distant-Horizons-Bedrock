#include "dh/storage/SourceUpdater.h"

#include "dh/build/LightingEngine.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/storage/SourceProvider.h"

#include <algorithm>
#include <utility>

namespace dh {

SourceUpdater::SourceUpdater(SourceProvider& provider, const UpdaterOptions& options)
: provider_(provider),
  options_(options) {}

Result<bool> SourceUpdater::update(std::shared_ptr<const FullDataSource> input) {
    if (!input) return Error(ErrorCode::InvalidArgument, "null input");
    if (shutdown_.load()) return false;

    // A lock is necessary to prevent two threads from writing to the same position at once,
    // otherwise only the second update would apply and the LOD ends up with holes (Java :101).
    auto guard = locks_.lock(input->pos);
    return updateLocked(*input);
}

Result<bool> SourceUpdater::updateLocked(const FullDataSource& input) {
    if (shutdown_.load()) return false;

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
    // (FullDataUpdaterV2:148). Block lights were populated at the chunk stage; sky light must be
    // recalculated here so overhangs / floating islands do not end up with 0 sky light.
    if (sec::detail(pos) == sec::SECTION_BLOCK_DETAIL_LEVEL) {
        bakeDataSourceSkyLight(*recipient, options_.maxSkyLight);
    }

    // Timestamps (Java: stamped by the SQL layer on upsert).
    const std::int64_t now        = provider_.clock().nowMillis();
    recipient->lastModifiedUnixMs = now;
    if (recipient->createdUnixMs == 0) recipient->createdUnixMs = now;

    // Save to the store.
    auto rec = encode(*recipient, options_.codec);
    if (!rec) return rec.error();
    if (Status st = provider_.store().put(*rec); !st) return st.error();

    provider_.publish(recipient);

    // Listeners (registration order), after the save, only on change.
    std::vector<UpdateListener> snapshot;
    {
        std::lock_guard lock(listenerMutex_);
        snapshot.reserve(listeners_.size());
        for (auto& l : listeners_) snapshot.push_back(l.second);
    }
    for (auto& l : snapshot) {
        if (l) l(pos);
    }
    return true;
}

std::shared_ptr<UpdateFuture> SourceUpdater::updateAsync(std::shared_ptr<const FullDataSource> input) {
    auto future = std::make_shared<UpdateFuture>();
    if (!input || shutdown_.load() || options_.asyncExecutor == nullptr) {
        future->complete(false);
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
        // Executor shut down while we queued (Java: RejectedExecutionException -> completed future).
        pendingAsync_.fetch_sub(1);
        future->complete(false);
    }
    return future;
}

ListenerId SourceUpdater::addListener(UpdateListener listener) {
    std::lock_guard  lock(listenerMutex_);
    const ListenerId id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void SourceUpdater::removeListener(ListenerId id) {
    std::lock_guard lock(listenerMutex_);
    std::erase_if(listeners_, [id](const auto& p) { return p.first == id; });
}

} // namespace dh
