#pragma once

// Mesh hand-off from the core (mesh builders on worker threads) to the platform
// renderer (Java: ColumnRenderBuffer / GLVertexBuffer upload).
//
// The core produces, per render section, a list of opaque and a list of
// transparent byte buffers. Each buffer holds whole quads: 4 vertices x 16 bytes
// = 64 bytes per quad, so buffer.size() % 64 == 0 and no buffer exceeds
// kMaxBufferBytes (split on a quad boundary, as Java's 2 MiB builder does).
//
// Vertex layout (WP-F1, byte offsets, little-endian):
//   0  int16  x        section-relative block position (may be negative/overhang)
//   2  int16  y
//   4  int16  z
//   6  uint16 meta     bits 0-3 sky light, 4-7 block light, 8-15 micro-offset/flags
//   8  uint8  r,g,b,a  colour already shaded and tinted (ARGB -> RGBA order)
//   12 uint8  material BlockMaterial value (0-15)
//   13 uint8  normal   Direction face index (0-5)
//   14 uint16 tile     texture tile id (0 = none; Stage A ignores it)
// Indices are implicit: quad i uses vertices 4i..4i+3 in
// DIRECTION_VERTEX_IBO_QUAD order; the renderer supplies a shared index buffer.
//
// Threading: upload()/remove() may be called from any core thread. The sink
// must not block on GPU work; it queues and completes asynchronously, invoking
// `onDone(true)` once the section is renderable or `onDone(false)` if the upload
// was dropped (renderer shut down, section removed first, out of memory).
// The callback may run on any thread, including synchronously inside upload().
//
// Ordering: for one sectionPos, calls are applied in call order; an upload that
// follows a remove re-creates the section, a remove after an upload drops it
// (pending uploads complete with false). Callback order is NOT constrained: the
// callback of upload k may fire after upload k+1 for the same pos was issued or
// even applied (batching sinks). Producers therefore identify their uploads with
// uploadWithId()/removeIf() instead of relying on callback order.
//
// Upload identity (WP-G2): uploadWithId() returns a sink-chosen UploadId for that
// upload; removeIf(pos, id) frees the section only if the newest upload the sink
// holds (applied or still pending) for `pos` carries `id`, and is a no-op for a
// stale id. The defaults keep older sinks compiling (ids are all 0 and removeIf
// degrades to remove()); real sinks should implement both (GLUE-4).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace dh {

inline constexpr std::size_t kVertexBytes    = 16;
inline constexpr std::size_t kQuadBytes      = 4 * kVertexBytes;
inline constexpr std::size_t kMaxBufferBytes = 2u * 1024u * 1024u; ///< Java LodQuadBuilder 2 MiB split

struct MeshBuffers {
    std::vector<std::vector<std::byte>> opaque;
    std::vector<std::vector<std::byte>> transparent;

    bool empty() const { return opaque.empty() && transparent.empty(); }

    std::size_t totalBytes() const {
        std::size_t n = 0;
        for (const auto& b : opaque) n += b.size();
        for (const auto& b : transparent) n += b.size();
        return n;
    }
};

struct IVertexSink {
    using UploadCallback = std::function<void(bool ok)>;
    /// Identity of one upload for `removeIf`; 0 = unknown (sink without identity support).
    using UploadId = std::uint64_t;

    virtual ~IVertexSink() = default;

    /// Replaces the geometry for `sectionPos` (WP-A encoded DhSectionPos).
    /// An empty MeshBuffers is a valid upload meaning "render nothing".
    virtual void upload(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) = 0;

    /// Stops rendering `sectionPos` and frees its GPU resources. No-op if absent.
    virtual void remove(std::uint64_t sectionPos) = 0;

    /// upload() that also returns the upload's identity (see header). Default: plain upload, id 0.
    virtual UploadId uploadWithId(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) {
        upload(sectionPos, std::move(buffers), std::move(onDone));
        return 0;
    }

    /// remove(sectionPos) only if the newest upload held for `sectionPos` (applied or pending) has `id`;
    /// a stale id (a newer upload exists) is ignored. Default: unconditional remove().
    virtual void removeIf(std::uint64_t sectionPos, UploadId /*id*/) { remove(sectionPos); }
};

/// Test double: records every call. Callbacks are deferred until the test calls
/// completeAll()/completeNext(), unless autoComplete is set.
class RecordingVertexSink final : public IVertexSink {
public:
    struct Upload {
        std::uint64_t  sectionPos;
        MeshBuffers    buffers;
        UploadCallback onDone;
        UploadId       id = 0;
    };

    bool autoComplete = false; ///< if true, onDone(true) fires inside upload()

    void upload(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) override {
        (void)uploadWithId(sectionPos, std::move(buffers), std::move(onDone));
    }

    /// Ids are monotonic (1, 2, ...); the newest id per pos is what removeIf compares against.
    UploadId uploadWithId(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) override {
        UploadCallback cb;
        UploadId       id;
        {
            std::lock_guard lock(mutex_);
            id = ++lastId_;
            uploads_.push_back(Upload{sectionPos, std::move(buffers), {}, id});
            newestIdByPos_[sectionPos] = id;
            if (autoComplete) cb = std::move(onDone);
            else uploads_.back().onDone = std::move(onDone);
        }
        if (cb) cb(true);
        return id;
    }

    void remove(std::uint64_t sectionPos) override {
        std::lock_guard lock(mutex_);
        removes_.push_back(sectionPos);
        newestIdByPos_.erase(sectionPos);
    }

    /// Removes only if `id` is the newest upload for the pos; stale ids are recorded in staleRemoves().
    void removeIf(std::uint64_t sectionPos, UploadId id) override {
        std::lock_guard lock(mutex_);
        auto            it = newestIdByPos_.find(sectionPos);
        if (it != newestIdByPos_.end() && it->second == id) {
            removes_.push_back(sectionPos);
            newestIdByPos_.erase(it);
        } else {
            staleRemoves_.push_back(sectionPos);
        }
    }

    /// Fires the oldest pending callback with `ok`. Returns false if none pending.
    bool completeNext(bool ok = true) {
        UploadCallback cb;
        {
            std::lock_guard lock(mutex_);
            for (auto& u : uploads_) {
                if (u.onDone) {
                    cb       = std::move(u.onDone);
                    u.onDone = nullptr;
                    break;
                }
            }
        }
        if (!cb) return false;
        cb(ok);
        return true;
    }

    std::size_t completeAll(bool ok = true) {
        std::size_t n = 0;
        while (completeNext(ok)) ++n;
        return n;
    }

    std::vector<Upload>&        uploads() { return uploads_; }
    std::vector<std::uint64_t>& removes() { return removes_; }
    /// removeIf calls that were ignored because a newer upload existed for the pos.
    std::vector<std::uint64_t>& staleRemoves() { return staleRemoves_; }
    /// Newest upload id for a pos, 0 if the pos holds nothing.
    UploadId newestId(std::uint64_t sectionPos) const {
        std::lock_guard lock(mutex_);
        auto            it = newestIdByPos_.find(sectionPos);
        return it == newestIdByPos_.end() ? 0 : it->second;
    }

    std::size_t pendingCount() const {
        std::lock_guard lock(mutex_);
        std::size_t     n = 0;
        for (const auto& u : uploads_) n += u.onDone ? 1 : 0;
        return n;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        uploads_.clear();
        removes_.clear();
        staleRemoves_.clear();
        newestIdByPos_.clear();
    }

private:
    mutable std::mutex                mutex_;
    std::vector<Upload>               uploads_;
    std::vector<std::uint64_t>        removes_;
    std::vector<std::uint64_t>        staleRemoves_;
    std::map<std::uint64_t, UploadId> newestIdByPos_;
    UploadId                          lastId_ = 0;
};

} // namespace dh
