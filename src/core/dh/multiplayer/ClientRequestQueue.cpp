#include "dh/multiplayer/ClientRequestQueue.h"

#include "dh/fulldata/FullDataSource.h"
#include "dh/multiplayer/WorldGenRange.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/util/Bits.h"

#include <algorithm>
#include <vector>

namespace dh::mp {

NetworkRequestQueue::NetworkRequestQueue(
    ClientNetworkState& net,
    std::string         levelName,
    IExecutor&          work,
    bool                changedOnly
)
: net_(net),
  levelName_(std::move(levelName)),
  work_(work),
  changedOnly_(changedOnly),
  rateLimiter_([this] { return requestRateLimit(); }, {}, net.clock()) {}

NetworkRequestQueue::~NetworkRequestQueue() {
    closeQueue();
    alive_.reset();
}

void NetworkRequestQueue::setBeaconSink(BeaconSink sink) {
    std::lock_guard lock(mutex_);
    beaconSink_ = std::move(sink);
}

std::size_t NetworkRequestQueue::waitingCount() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

std::size_t NetworkRequestQueue::inProgressCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(inProgress_);
}

bool NetworkRequestQueue::requestPosExistsWhere(const std::function<bool(std::uint64_t)>& pred) const {
    std::vector<std::uint64_t> positions;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [pos, t] : tasks_) positions.push_back(pos);
    }
    return std::any_of(positions.begin(), positions.end(), pred);
}

//------------------------------------------------------------------------------
// submitting
//------------------------------------------------------------------------------

std::shared_ptr<RetrievalFuture>
NetworkRequestQueue::submitRequest(std::uint64_t sectionPos, std::optional<std::int64_t> clientTimestamp) {
    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(mutex_);
        if (closing_.load()) {
            auto f = std::make_shared<RetrievalFuture>();
            f->cancel();
            return f;
        }
        auto& slot = tasks_[sectionPos];
        if (slot) return slot->future; // ignore already queued tasks
        slot                  = std::make_shared<Task>();
        slot->pos             = sectionPos;
        slot->future          = std::make_shared<RetrievalFuture>();
        slot->updateTimestamp = clientTimestamp;
        task                  = slot;
    }
    std::weak_ptr<bool> weak = alive_;
    task->future->onComplete([this, weak, task](const RetrievalResult& r) {
        auto a = weak.lock();
        if (!a) return;
        onTaskCompleted(task, r);
    });
    return task->future;
}

void NetworkRequestQueue::onTaskCompleted(const std::shared_ptr<Task>& task, const RetrievalResult& result) {
    std::uint64_t cancelId = 0;
    {
        std::lock_guard lock(mutex_);
        auto            it = tasks_.find(task->pos);
        if (it != tasks_.end() && it->second == task) tasks_.erase(it);
        if (task->inFlight) {
            // the requester gave up while the server was still working: free the slot, tell the server
            task->inFlight = false;
            --inProgress_;
            cancelId          = task->netFutureId;
            task->netFutureId = 0;
        }
    }
    if (cancelId != 0) net_.cancelRequest(cancelId);
    switch (result.kind) {
    case RetrievalResult::Kind::Success:
        finished_.fetch_add(1);
        break;
    case RetrievalResult::Kind::Failed:
        failed_.fetch_add(1);
        break;
    case RetrievalResult::Kind::Split:
    case RetrievalResult::Kind::Rejected:
        break;
    }
}

//------------------------------------------------------------------------------
// ticking
//------------------------------------------------------------------------------

bool NetworkRequestQueue::tick(BlockPos2D target) {
    // futures are cancelled after tickMutex_ is released: their listeners may call back into the queue
    std::vector<std::shared_ptr<Task>> toCancel;
    const bool                         ok = tickLocked(target, toCancel);
    for (auto& t : toCancel) t->future->cancel();
    return ok;
}

bool NetworkRequestQueue::tickLocked(BlockPos2D target, std::vector<std::shared_ptr<Task>>& toCancel) {
    std::lock_guard tickLock(tickMutex_);
    if (closing_.load() || !net_.isReady()) return false;
    {
        std::lock_guard lock(mutex_);
        lastTarget_ = target;
    }

    // queue requests until the queue is full
    std::size_t budget;
    {
        std::lock_guard lock(mutex_);
        budget = tasks_.size() + 1; // every iteration sends or removes a task; this only guards against spinning
    }
    while (budget-- > 0) {
        std::size_t waiting;
        int         inProgress;
        {
            std::lock_guard lock(mutex_);
            waiting    = tasks_.size();
            inProgress = inProgress_;
        }
        if (!(static_cast<std::size_t>(inProgress) < waiting && inProgress < requestRateLimit())) break;
        if (!rateLimiter_.tryAcquire()) break;
        if (!sendNext(target, toCancel)) break;
    }
    return true;
}

bool NetworkRequestQueue::sendNext(BlockPos2D target, std::vector<std::shared_ptr<Task>>& toCancel) {
    std::shared_ptr<Task> task;
    {
        // nearest first; ties go to the lowest pos key (map order) so the choice is deterministic
        std::lock_guard lock(mutex_);
        std::int32_t    best = 0;
        for (const auto& [pos, t] : tasks_) {
            if (t->inFlight || std::find(toCancel.begin(), toCancel.end(), t) != toCancel.end()) continue;
            const std::int32_t d = sec::chebyshevSignedDistance(pos, target.x, target.z);
            if (!task || d < best) {
                task = t;
                best = d;
            }
        }
    }
    if (!task) return false;

    if (!sectionInAllowedGenerationRadius(task->pos, target)) {
        toCancel.push_back(task); // cancelled by tick() once the lock is released; onTaskCompleted removes it
        return true;
    }

    std::uint64_t               attempt;
    std::optional<std::int64_t> offsetTimestamp;
    {
        std::lock_guard lock(mutex_);
        auto            it = tasks_.find(task->pos);
        if (it == tasks_.end() || it->second != task || task->inFlight) return false;
        task->inFlight = true;
        attempt = task->attempt = nextAttempt_++;
        ++inProgress_;
        if (task->updateTimestamp) offsetTimestamp = *task->updateTimestamp + net_.serverTimeOffset();
    }

    std::weak_ptr<bool> weak = alive_;
    const std::uint64_t id   = net_.sendRequest(
        levelName_,
        task->pos,
        offsetTimestamp,
        [this, weak, task, attempt](const NetResponse& response) {
            auto a = weak.lock();
            if (!a) return;
            const bool accepted = work_.execute([this, weak, task, attempt, response] {
                auto b = weak.lock();
                if (!b) return;
                handleResponse(task, attempt, response);
            });
            if (!accepted) task->future->cancel();
        }
    );

    bool stale = false;
    {
        std::lock_guard lock(mutex_);
        if (task->inFlight && task->attempt == attempt) task->netFutureId = id;
        else stale = true; // finished or cancelled before the id was known
    }
    if (stale && id != 0) net_.cancelRequest(id);
    return true;
}

//------------------------------------------------------------------------------
// responses
//------------------------------------------------------------------------------

void NetworkRequestQueue::retryOrFail(const std::shared_ptr<Task>& task, const std::string& why) {
    bool again;
    {
        std::lock_guard lock(mutex_);
        --task->retryAttempts;
        again = task->retryAttempts > 0;
    }
    log::logf(
        LogLevel::Warn,
        "network request for pos %llu failed (%s)",
        static_cast<unsigned long long>(task->pos),
        why.c_str()
    );
    if (!again) task->future->complete(RetrievalResult::failed(why)); // otherwise the next tick resends it
}

void NetworkRequestQueue::handleResponse(
    const std::shared_ptr<Task>& task,
    std::uint64_t                attempt,
    const NetResponse&           response
) {
    {
        std::lock_guard lock(mutex_);
        if (!task->inFlight || task->attempt != attempt) return; // cancelled meanwhile
        task->inFlight    = false;
        task->netFutureId = 0;
        --inProgress_;
    }

    switch (response.kind) {
    case NetResponse::Kind::Closed:
        task->future->cancel(); // SessionClosedException
        return;

    case NetResponse::Kind::Empty:
        if (changedOnly_) task->future->cancel(); // nothing newer on the server
        else retryOrFail(task, "Received empty data source response for not changes-only request");
        return;

    case NetResponse::Kind::Remote:
        switch (response.exception) {
        case net::RemoteException::SectionRequiresSplitting:
            task->future->complete(RetrievalResult::split());
            return;
        case net::RemoteException::RateLimited:
            rateLimiter_.acquireAll(); // skip all requests for 1 second; the task is re-queued
            return;
        case net::RemoteException::RequestOutOfRange:
            return; // re-queued
        case net::RemoteException::RequestRejected:
            task->future->complete(RetrievalResult::failed(response.message));
            return;
        }
        return;

    case NetResponse::Kind::Payload: {
        auto decoded = net_.decodePayload(response.payload);
        if (!decoded) {
            retryOrFail(task, decoded.error().message);
            return;
        }
        FullDataSource source = std::move(*decoded);
        if (source.pos != task->pos) {
            retryOrFail(task, "response position does not match the request");
            return;
        }
        // set application flags based on the received detail level so the data propagates correctly
        const int detail       = sec::detail(source.pos);
        source.applyToChildren = detail > sec::SECTION_BLOCK_DETAIL_LEVEL;
        source.applyToParent   = detail < sec::SECTION_BLOCK_DETAIL_LEVEL + 12;

        BeaconSink sink;
        {
            std::lock_guard lock(mutex_);
            sink = beaconSink_;
        }
        if (sink) sink(source.pos, response.payload.beaconBeams);

        const std::uint64_t pos = source.pos;
        task->future->complete(RetrievalResult::success(pos, std::make_shared<FullDataSource>(std::move(source))));
        return;
    }
    }
}

//------------------------------------------------------------------------------
// removal / shutdown
//------------------------------------------------------------------------------

void NetworkRequestQueue::removeRetrievalRequestIf(const std::function<bool(std::uint64_t)>& removeIf) {
    // furthest tasks first
    std::vector<std::pair<std::int32_t, std::shared_ptr<Task>>> ordered;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [pos, t] : tasks_)
            ordered.emplace_back(sec::chebyshevSignedDistance(pos, lastTarget_.x, lastTarget_.z), t);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second->pos > b.second->pos;
    });
    for (auto& [dist, task] : ordered) {
        if (removeIf(task->pos)) task->future->cancel(); // onTaskCompleted also cancels the server request
    }
}

void NetworkRequestQueue::closeQueue() {
    closing_.store(true);
    std::vector<std::shared_ptr<Task>> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [pos, t] : tasks_) all.push_back(t);
    }
    for (auto& t : all) t->future->cancel();
}

//------------------------------------------------------------------------------
// RemoteWorldRetrievalQueue
//------------------------------------------------------------------------------

ClientRequestQueue::ClientRequestQueue(ClientNetworkState& net, std::string levelName, IExecutor& work)
: NetworkRequestQueue(net, std::move(levelName), work, /*changedOnly=*/false) {}

ClientRequestQueue::~ClientRequestQueue() { closeQueue(); }

std::shared_ptr<RetrievalFuture> ClientRequestQueue::submit(std::uint64_t sectionPos, std::uint8_t) {
    return submitRequest(sectionPos, std::nullopt);
}

void ClientRequestQueue::startAndSetTargetPos(BlockPos2D target) { (void)NetworkRequestQueue::tick(target); }

void ClientRequestQueue::shutdown(bool) { closeQueue(); }

int ClientRequestQueue::requestRateLimit() const { return net_.sessionConfig().generationRequestRateLimit; }

bool ClientRequestQueue::sectionInAllowedGenerationRadius(std::uint64_t pos, BlockPos2D target) const {
    const net::SessionConfigValues cfg = net_.sessionConfig();
    if (cfg.generationMaxChunkRadius > 0) {
        if (!isPosInWorldGenRange(
                pos,
                cfg.generationCenterChunkX,
                cfg.generationCenterChunkZ,
                cfg.generationMaxChunkRadius
            ))
            return false;
    }
    return sec::chebyshevSignedDistance(pos, target.x, target.z) <= wmul(cfg.maxGenerationRequestDistance, 16);
}

//------------------------------------------------------------------------------
// SyncOnLoadRequestQueue
//------------------------------------------------------------------------------

SyncOnLoadRequestQueue::SyncOnLoadRequestQueue(ClientNetworkState& net, std::string levelName, IExecutor& work)
: NetworkRequestQueue(net, std::move(levelName), work, /*changedOnly=*/true) {}

SyncOnLoadRequestQueue::~SyncOnLoadRequestQueue() { closeQueue(); }

bool SyncOnLoadRequestQueue::tick(BlockPos2D targetPos) {
    if (!net_.sessionConfig().synchronizeOnLoad) return false;
    return NetworkRequestQueue::tick(targetPos);
}

int SyncOnLoadRequestQueue::requestRateLimit() const { return net_.sessionConfig().syncOnLoadRateLimit; }

bool SyncOnLoadRequestQueue::sectionInAllowedGenerationRadius(std::uint64_t pos, BlockPos2D target) const {
    return sec::chebyshevSignedDistance(pos, target.x, target.z)
        <= wmul(net_.sessionConfig().maxSyncOnLoadRequestDistance, 16);
}

} // namespace dh::mp
