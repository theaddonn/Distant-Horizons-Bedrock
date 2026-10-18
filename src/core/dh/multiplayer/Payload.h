#pragma once

// Port of
// multiplayer/fullData/{FullDataPayload,FullDataPayloadSender,FullDataPayloadReceiver,SharedBandwidthLimit}.java.
//
// A full-data transfer is: N x FullDataSplit frames carrying the Codec record bytes, then ONE
// FullDataSourceResponse / FullDataPartialUpdate message naming the payload (bufferId + beacons).
// The sender meters the splits per tick; the final message is only sent after the last split.
//
// Differences from Java (also listed in the WP-K2 report):
//   * chunk size is derived from ITransport::maxFrameBytes() (Java: fixed 1,048,000);
//   * a sender counts towards the shared bandwidth split only while it has queued transfers
//     (Java: every sender that ever ticked, i.e. idle players diluted the global limit);
//   * a player limit of 0 (unlimited) no longer disables the global limit (Java: min(0, share) == 0),
//     and a global share that rounds to 0 is raised to 1 KB/s;
//   * transport failures drop the transfer and run the failure callback (Java closed the session).
//
// Nothing here owns a thread: the owner calls PayloadSender::tick() at 20 Hz.

#include "dh/fulldata/FullDataSource.h"
#include "dh/net/Messages.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IClock.h"
#include "dh/platform/ITransport.h"
#include "dh/storage/Codec.h"
#include "dh/util/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace dh::mp {

inline constexpr int kPayloadTickRate = 20; // FullDataPayloadSender.TICK_RATE

/// version(2) + id(2) + bufferId(4) + size(4) + isFirst(1): the bytes a FullDataSplit frame adds to its data.
inline constexpr std::size_t kSplitFrameOverhead = 13;

/// FullDataPayloadReceiver: expireAfterAccess(30 s).
inline constexpr std::int64_t kReceiverExpiryNanos = 30LL * 1'000'000'000LL;

/// A buffer larger than this is dropped (a decoder would reject it anyway).
inline constexpr std::size_t kMaxPayloadBytes = kCodecMaxBlobBytes;
/// Caps over all buffers of one receiver; the least recently used buffers are evicted beyond them.
inline constexpr std::size_t kMaxReceiverBuffers    = 256;
inline constexpr std::size_t kMaxReceiverTotalBytes = 256u * 1024u * 1024u;

/// The part of Java's FullDataPayload that is not on the wire in the message itself.
struct FullDataPayload {
    std::int32_t                 bufferId = 0;
    std::vector<std::byte>       bytes; ///< Codec::writeRecord output
    std::vector<net::BeaconBeam> beaconBeams;

    /// The wire form carried by FullDataSourceResponse / FullDataPartialUpdate.
    net::FullDataPayload header() const { return net::FullDataPayload{bufferId, beaconBeams}; }
};

/// Java FullDataPayload.lastBufferId.
class BufferIdAllocator {
public:
    std::int32_t allocate() { return next_.fetch_add(1); }

private:
    std::atomic<std::int32_t> next_{0};
};

/// encode (Codec) + writeRecord.
Result<FullDataPayload> makePayload(
    const FullDataSource&        source,
    std::vector<net::BeaconBeam> beacons,
    std::int32_t                 bufferId,
    const CodecOptions&          codec = {}
);

/// readRecord (must consume every byte) + decode.
Result<FullDataSource> decodePayloadBytes(std::span<const std::byte> bytes, IBlockRegistry& registry);

//------------------------------------------------------------------------------
// SharedBandwidthLimit
//------------------------------------------------------------------------------

class BandwidthLimiter {
public:
    /// `globalKBps`: Config.Server.globalBandwidthLimit (KB/s, 0 = unlimited).
    explicit BandwidthLimiter(std::function<int()> globalKBps) : global_(std::move(globalKBps)) {}

    void        setSenderActive(const void* sender, bool active);
    std::size_t activeSenders() const;

    /// KB/s one active sender may use: global / max(activeSenders, 1); 0 = unlimited.
    int bandwidthShare() const;

private:
    std::function<int()>  global_;
    mutable std::mutex    mutex_;
    std::set<const void*> senders_;
};

//------------------------------------------------------------------------------
// FullDataPayloadSender
//------------------------------------------------------------------------------

class PayloadSender {
public:
    using Supplier = std::function<int()>;

    PayloadSender(ITransport& transport, SessionId session, Supplier playerKBps, BandwidthLimiter& shared);
    ~PayloadSender();
    PayloadSender(const PayloadSender&)            = delete;
    PayloadSender& operator=(const PayloadSender&) = delete;

    /// Queues the transfer. `finalMessage` runs right after the last split frame was sent;
    /// `onFailure` runs instead if the transfer is dropped (transport refused a frame, close()).
    void sendInChunks(
        std::shared_ptr<const FullDataPayload> payload,
        std::function<void()>                  finalMessage,
        std::function<void()>                  onFailure = {}
    );

    /// One 20 Hz tick: sends up to `bytesPerTick(rate)` payload bytes.
    void tick();
    /// Same with a caller-supplied bandwidth share (snapshot once per pass over all senders).
    void tick(int shareKBps);

    /// Drops every queued transfer (running onFailure) and leaves the shared limit.
    void close();

    std::size_t pendingTransfers() const;
    /// Data bytes per FullDataSplit frame: maxFrameBytes() - kSplitFrameOverhead (at least 1).
    std::size_t   chunkSize() const { return chunkSize_; }
    std::uint64_t splitFramesSent() const { return framesSent_.load(); }
    std::uint64_t failedTransfers() const { return failed_.load(); }

    /// rate*1000/20 + 1 (the +1 absorbs rounding for tiny rates); rate <= 0 -> unlimited (INT64_MAX).
    static std::int64_t bytesPerTick(int rateKBps);
    /// min(player, share) where 0 means unlimited on either side.
    static int effectiveRate(int playerKBps, int shareKBps);

private:
    struct Transfer {
        std::shared_ptr<const FullDataPayload> payload;
        std::size_t                            offset    = 0;
        bool                                   firstSent = false;
        std::function<void()>                  finalMessage;
        std::function<void()>                  onFailure;
    };

    ITransport&       transport_;
    SessionId         session_;
    Supplier          playerKBps_;
    BandwidthLimiter& shared_;
    std::size_t       chunkSize_;

    mutable std::mutex   mutex_;
    std::deque<Transfer> queue_;

    std::atomic<std::uint64_t> framesSent_{0};
    std::atomic<std::uint64_t> failed_{0};
};

//------------------------------------------------------------------------------
// FullDataPayloadReceiver
//------------------------------------------------------------------------------

class PayloadReceiver {
public:
    explicit PayloadReceiver(const IClock& clock, std::int64_t expiryNanos = kReceiverExpiryNanos)
    : clock_(clock),
      expiryNanos_(expiryNanos) {}

    /// isFirst starts (or restarts) the buffer; a non-first chunk without a buffer is dropped.
    void receiveChunk(const net::FullDataSplitMessage& message);

    bool has(std::int32_t bufferId);
    /// Removes and returns the buffer (nullopt if unknown or expired).
    std::optional<std::vector<std::byte>> take(std::int32_t bufferId);
    void                                  discard(std::int32_t bufferId);

    /// take() + decodePayloadBytes(). The buffer is removed even when decoding fails (Java: finally remove).
    Result<FullDataSource> decode(std::int32_t bufferId, IBlockRegistry& registry);

    /// Drops buffers not accessed for the expiry time. Also runs inside every other call.
    std::size_t expire();

    std::size_t   bufferCount();
    std::uint64_t droppedChunks() const { return dropped_.load(); }

private:
    struct Buffer {
        std::vector<std::byte> bytes;
        std::int64_t           lastAccessNanos = 0;
    };
    std::size_t expireLocked(std::int64_t now);

    const IClock&                  clock_;
    std::int64_t                   expiryNanos_;
    std::mutex                     mutex_;
    std::map<std::int32_t, Buffer> buffers_;
    std::atomic<std::uint64_t>     dropped_{0};
};

} // namespace dh::mp
