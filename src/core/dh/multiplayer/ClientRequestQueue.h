#pragma once

// Port of multiplayer/client/{AbstractFullDataNetworkRequestQueue,SyncOnLoadRequestQueue}.java and
// generation/queues/RemoteWorldRetrievalQueue.java.
//
//   NetworkRequestQueue      shared machinery: waiting tasks by section pos, in-flight cap
//                            (min(rate limit, waiting)), rate limiter, nearest-first sending,
//                            distance gate, retries (3), response -> RetrievalResult mapping
//   ClientRequestQueue       generation requests (no client timestamp); an IRetrievalQueue, so the
//                            LodQuadTree/LodRequestModule use the network exactly like world gen
//   SyncOnLoadRequestQueue   changed-only requests carrying the client's timestamp
//
// The queues are driven: IRetrievalQueue::startAndSetTargetPos / SyncOnLoadRequestQueue::tick send
// requests; responses are decoded on the injected `work` executor.
//
// Deviations: an "in-flight semaphore" is a counter released exactly once per request (Java could
// release twice around cancellation); futures use RetrievalFuture (cancel -> Kind::Rejected,
// exceptional completion -> Kind::Failed); close() is synchronous (Java waited up to 5 s).

#include "dh/gen/RetrievalQueue.h"
#include "dh/multiplayer/ClientNetworkState.h"
#include "dh/platform/IExecutor.h"
#include "dh/pos/BlockPos.h"
#include "dh/thread/RateLimiter.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dh::mp {

inline constexpr int kMaxRetryAttempts = 3; // AbstractFullDataNetworkRequestQueue.MAX_RETRY_ATTEMPTS

class NetworkRequestQueue {
public:
    using BeaconSink = std::function<void(std::uint64_t pos, const std::vector<net::BeaconBeam>&)>;

    NetworkRequestQueue(ClientNetworkState& net, std::string levelName, IExecutor& work, bool changedOnly);
    virtual ~NetworkRequestQueue();
    NetworkRequestQueue(const NetworkRequestQueue&)            = delete;
    NetworkRequestQueue& operator=(const NetworkRequestQueue&) = delete;

    /// A pos already waiting returns its future. `clientTimestamp` is in client time; the server clock
    /// offset is added when sending.
    std::shared_ptr<RetrievalFuture>
    submitRequest(std::uint64_t sectionPos, std::optional<std::int64_t> clientTimestamp);

    /// Sends requests until the in-flight cap, the rate limit or the waiting set stops it.
    /// false when closed or the session is not ready.
    virtual bool tick(BlockPos2D targetPos);

    /// Cancels waiting AND in-flight requests whose pos matches, furthest from the target first.
    void removeRetrievalRequestIf(const std::function<bool(std::uint64_t)>& removeIf);
    bool requestPosExistsWhere(const std::function<bool(std::uint64_t)>& pred) const;

    std::size_t waitingCount() const;
    std::size_t inProgressCount() const;
    int         finishedRequests() const { return finished_.load(); }
    int         failedRequests() const { return failed_.load(); }

    /// Cancels everything (sending Cancel for in-flight requests); further ticks do nothing.
    void closeQueue();
    bool isClosed() const { return closing_.load(); }

    /// updateBeaconBeamsForSectionPos
    void setBeaconSink(BeaconSink sink);

    virtual const char* queueName() const = 0;

protected:
    virtual int  requestRateLimit() const                                                     = 0;
    virtual bool sectionInAllowedGenerationRadius(std::uint64_t pos, BlockPos2D target) const = 0;

    ClientNetworkState& net_;

private:
    struct Task {
        std::uint64_t                    pos = 0;
        std::shared_ptr<RetrievalFuture> future;
        std::optional<std::int64_t>      updateTimestamp;
        bool                             inFlight      = false;
        std::uint64_t                    netFutureId   = 0;
        std::uint64_t                    attempt       = 0;
        int                              retryAttempts = kMaxRetryAttempts;
    };

    bool sendNext(BlockPos2D target);
    void handleResponse(const std::shared_ptr<Task>& task, std::uint64_t attempt, const NetResponse& response);
    void retryOrFail(const std::shared_ptr<Task>& task, const std::string& why);
    void onTaskCompleted(const std::shared_ptr<Task>& task, const RetrievalResult& result);

    std::string levelName_;
    IExecutor&  work_;
    bool        changedOnly_;

    std::shared_ptr<bool>         alive_ = std::make_shared<bool>(true);
    std::atomic<bool>             closing_{false};
    std::atomic<int>              finished_{0};
    std::atomic<int>              failed_{0};
    SupplierBasedRateLimiter<int> rateLimiter_;

    std::mutex                                     tickMutex_; ///< Java: synchronized tick()
    mutable std::mutex                             mutex_;
    std::map<std::uint64_t, std::shared_ptr<Task>> tasks_; ///< waiting + in flight, by pos
    int                                            inProgress_  = 0;
    std::uint64_t                                  nextAttempt_ = 1;
    BlockPos2D                                     lastTarget_;
    BeaconSink                                     beaconSink_;
};

/// RemoteWorldRetrievalQueue.
class ClientRequestQueue final : public NetworkRequestQueue, public IRetrievalQueue {
public:
    ClientRequestQueue(ClientNetworkState& net, std::string levelName, IExecutor& work);
    ~ClientRequestQueue() override;

    // IRetrievalQueue
    std::shared_ptr<RetrievalFuture> submit(std::uint64_t sectionPos, std::uint8_t requiredGenDetail) override;
    void                             startAndSetTargetPos(BlockPos2D target) override;
    void                             removeRetrievalRequestIf(std::function<bool(std::uint64_t)> removeIf) override {
        NetworkRequestQueue::removeRetrievalRequestIf(removeIf);
    }
    std::size_t waitingTaskCount() const override { return waitingCount(); }
    std::size_t inProgressTaskCount() const override { return inProgressCount(); }
    void        shutdown(bool interruptRunning) override;
    void        setCanRegenerate(bool v) override { canRegenerate_.store(v); }
    bool        getCanRegenerate() const override { return canRegenerate_.load(); }

    const char* queueName() const override { return "World Remote Generation Queue"; }

protected:
    int  requestRateLimit() const override;
    bool sectionInAllowedGenerationRadius(std::uint64_t pos, BlockPos2D target) const override;

private:
    std::atomic<bool> canRegenerate_{false};
};

/// SyncOnLoadRequestQueue.
class SyncOnLoadRequestQueue final : public NetworkRequestQueue {
public:
    SyncOnLoadRequestQueue(ClientNetworkState& net, std::string levelName, IExecutor& work);
    ~SyncOnLoadRequestQueue() override;

    bool tick(BlockPos2D targetPos) override;

    const char* queueName() const override { return "Sync On Login Queue"; }

protected:
    int  requestRateLimit() const override;
    bool sectionInAllowedGenerationRadius(std::uint64_t pos, BlockPos2D target) const override;
};

} // namespace dh::mp
