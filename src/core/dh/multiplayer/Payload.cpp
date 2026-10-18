#include "dh/multiplayer/Payload.h"

#include "dh/net/MessageRegistry.h"
#include "dh/platform/ILogger.h"
#include "dh/util/ByteIO.h"

#include <algorithm>
#include <limits>

namespace dh::mp {

Result<FullDataPayload> makePayload(
    const FullDataSource&        source,
    std::vector<net::BeaconBeam> beacons,
    std::int32_t                 bufferId,
    const CodecOptions&          codec
) {
    auto rec = encode(source, codec);
    if (!rec) return rec.error();
    FullDataPayload out;
    out.bufferId = bufferId;
    ByteWriter w(out.bytes);
    writeRecord(w, *rec);
    out.beaconBeams = std::move(beacons);
    return out;
}

Result<FullDataSource> decodePayloadBytes(std::span<const std::byte> bytes, IBlockRegistry& registry) {
    ByteReader r(bytes);
    auto       rec = readRecord(r);
    if (!rec) return rec.error();
    if (!r.atEnd()) return Error(ErrorCode::Corrupt, "trailing bytes after full data record");
    return decode(*rec, registry);
}

//------------------------------------------------------------------------------

void BandwidthLimiter::setSenderActive(const void* sender, bool active) {
    std::lock_guard lock(mutex_);
    if (active) senders_.insert(sender);
    else senders_.erase(sender);
}

std::size_t BandwidthLimiter::activeSenders() const {
    std::lock_guard lock(mutex_);
    return senders_.size();
}

int BandwidthLimiter::bandwidthShare() const {
    const int global = global_ ? global_() : 0;
    if (global <= 0) return 0; // Java: 0 -> Integer.MAX_VALUE (unlimited)
    const int n = static_cast<int>(std::max<std::size_t>(activeSenders(), 1));
    return std::max(global / n, 1); // never round an enabled limit down to "unlimited"
}

//------------------------------------------------------------------------------

PayloadSender::PayloadSender(ITransport& transport, SessionId session, Supplier playerKBps, BandwidthLimiter& shared)
: transport_(transport),
  session_(session),
  playerKBps_(std::move(playerKBps)),
  shared_(shared) {
    const std::size_t mtu = transport.maxFrameBytes();
    chunkSize_            = mtu > kSplitFrameOverhead ? mtu - kSplitFrameOverhead : 1;
}

PayloadSender::~PayloadSender() { close(); }

std::int64_t PayloadSender::bytesPerTick(int rateKBps) {
    if (rateKBps <= 0) return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(rateKBps) * 1000 / kPayloadTickRate + 1; // FullDataPayloadSender.java:60
}

int PayloadSender::effectiveRate(int playerKBps, int shareKBps) {
    if (playerKBps <= 0) return std::max(shareKBps, 0);
    if (shareKBps <= 0) return playerKBps;
    return std::min(playerKBps, shareKBps);
}

void PayloadSender::sendInChunks(
    std::shared_ptr<const FullDataPayload> payload,
    std::function<void()>                  finalMessage,
    std::function<void()>                  onFailure
) {
    if (!payload) {
        if (onFailure) onFailure();
        return;
    }
    {
        std::lock_guard lock(mutex_);
        Transfer        t;
        t.payload      = std::move(payload);
        t.finalMessage = std::move(finalMessage);
        t.onFailure    = std::move(onFailure);
        queue_.push_back(std::move(t));
    }
    // counted from the moment there is work, so shares do not depend on tick order
    shared_.setSenderActive(this, true);
}

std::size_t PayloadSender::pendingTransfers() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void PayloadSender::tick() {
    const int    rate   = effectiveRate(playerKBps_ ? playerKBps_() : 0, shared_.bandwidthShare());
    std::int64_t budget = bytesPerTick(rate);

    while (budget > 0) {
        std::shared_ptr<const FullDataPayload> payload;
        std::size_t                            offset  = 0;
        bool                                   isFirst = false;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) break;
            const Transfer& t = queue_.front();
            payload           = t.payload;
            offset            = t.offset;
            isFirst           = !t.firstSent;
        }

        const std::size_t readable = payload->bytes.size() - offset;
        std::int64_t      want     = std::min<std::int64_t>(budget, static_cast<std::int64_t>(chunkSize_));
        want                       = std::min<std::int64_t>(want, static_cast<std::int64_t>(readable));
        const std::size_t chunk    = static_cast<std::size_t>(want);

        net::FullDataSplitMessage msg;
        msg.bufferId = payload->bufferId;
        msg.isFirst  = isFirst;
        msg.buffer.assign(
            payload->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            payload->bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk)
        );
        const std::vector<std::byte> frame = net::encodeFrame(msg);

        if (!transport_.send(session_, frame)) {
            std::function<void()> onFailure;
            {
                std::lock_guard lock(mutex_);
                onFailure = std::move(queue_.front().onFailure);
                queue_.pop_front();
            }
            failed_.fetch_add(1);
            log::warn("payload sender: transport refused a split frame, dropping transfer");
            if (onFailure) onFailure();
            continue;
        }
        framesSent_.fetch_add(1);
        budget -= static_cast<std::int64_t>(chunk);

        std::function<void()> finalMessage;
        bool                  done = false;
        {
            std::lock_guard lock(mutex_);
            Transfer&       t  = queue_.front();
            t.firstSent        = true;
            t.offset          += chunk;
            if (t.offset >= t.payload->bytes.size()) {
                done         = true;
                finalMessage = std::move(t.finalMessage);
                queue_.pop_front();
            }
        }
        if (done && finalMessage) finalMessage(); // after the last split, before any later transfer
    }

    bool empty;
    {
        std::lock_guard lock(mutex_);
        empty = queue_.empty();
    }
    if (empty) shared_.setSenderActive(this, false);
}

void PayloadSender::close() {
    std::deque<Transfer> dropped;
    {
        std::lock_guard lock(mutex_);
        dropped.swap(queue_);
    }
    shared_.setSenderActive(this, false);
    for (Transfer& t : dropped) {
        if (t.onFailure) t.onFailure();
    }
}

//------------------------------------------------------------------------------

std::size_t PayloadReceiver::expireLocked(std::int64_t now) {
    std::size_t removed = 0;
    for (auto it = buffers_.begin(); it != buffers_.end();) {
        if (now - it->second.lastAccessNanos > expiryNanos_) {
            it = buffers_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t PayloadReceiver::expire() {
    std::lock_guard lock(mutex_);
    return expireLocked(clock_.steadyNanos());
}

std::size_t PayloadReceiver::bufferCount() {
    std::lock_guard lock(mutex_);
    expireLocked(clock_.steadyNanos());
    return buffers_.size();
}

void PayloadReceiver::receiveChunk(const net::FullDataSplitMessage& message) {
    std::lock_guard    lock(mutex_);
    const std::int64_t now = clock_.steadyNanos();
    expireLocked(now);

    auto it = buffers_.find(message.bufferId);
    if (message.isFirst) {
        // Java: a first chunk always creates a fresh composite buffer
        Buffer& fresh = buffers_[message.bufferId];
        fresh.bytes.clear();
        fresh.lastAccessNanos = now;
        it                    = buffers_.find(message.bufferId);
    } else if (it == buffers_.end()) {
        dropped_.fetch_add(1); // non-first chunk for an unknown buffer
        return;
    }
    Buffer& b = it->second;
    if (b.bytes.size() + message.buffer.size() > kMaxPayloadBytes) {
        buffers_.erase(it);
        dropped_.fetch_add(1);
        return;
    }
    b.bytes.insert(b.bytes.end(), message.buffer.begin(), message.buffer.end());
    b.lastAccessNanos = now;
}

bool PayloadReceiver::has(std::int32_t bufferId) {
    std::lock_guard lock(mutex_);
    expireLocked(clock_.steadyNanos());
    return buffers_.count(bufferId) != 0;
}

std::optional<std::vector<std::byte>> PayloadReceiver::take(std::int32_t bufferId) {
    std::lock_guard lock(mutex_);
    expireLocked(clock_.steadyNanos());
    auto it = buffers_.find(bufferId);
    if (it == buffers_.end()) return std::nullopt;
    std::vector<std::byte> out = std::move(it->second.bytes);
    buffers_.erase(it);
    return out;
}

void PayloadReceiver::discard(std::int32_t bufferId) {
    std::lock_guard lock(mutex_);
    buffers_.erase(bufferId);
}

Result<FullDataSource> PayloadReceiver::decode(std::int32_t bufferId, IBlockRegistry& registry) {
    auto bytes = take(bufferId);
    if (!bytes) return Error(ErrorCode::NotFound, "no complete buffer for a received payload");
    return decodePayloadBytes(*bytes, registry);
}

} // namespace dh::mp
