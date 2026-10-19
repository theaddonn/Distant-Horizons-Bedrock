#pragma once

// Stage-A render backend: IVertexSink that turns DH quad buffers into mce::Mesh objects.
//
// Threading:
//   upload()/remove()   any thread. The CPU conversion (dh::convertDhBuffer) runs on the calling thread; only a queue
//                       push happens under the lock. Per section the newest queued operation wins (older queued
//                       uploads complete with onDone(false), as the IVertexSink contract requires).
//   pump()              render thread only. Turns queued uploads into mce::Mesh (opaque + transparent, split into
//                       parts of <= 65000 vertices), replaces/destroys meshes. mce::Mesh objects are created and
//                       destroyed on the render thread only.
//   sections()          render thread only (read-only iteration by LodRenderer between pumps).

#include "dh/mesh/StageAVertex.h"
#include "dh/platform/IVertexSink.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class ScreenContext;
namespace mce {
class Mesh;
struct BufferResourceService;
} // namespace mce

namespace dh::render {

/// Conversion settings. Changing them (setSettings) re-converts every live section from its retained DH buffers on the
/// render thread, budgeted by pump().
struct MeshSettings {
    Primitive     prim = Primitive::QuadList;
    ConvertParams params{};
    float         yOffsetBlocks = -64.0f; ///< added to every DH y (DH y is relative to the level's min height)
};

class MeshUploader final : public dh::IVertexSink {
public:
    struct Part {
        std::unique_ptr<mce::Mesh> mesh;
        std::uint32_t              verts = 0;
    };
    struct Section {
        std::uint64_t                      pos  = 0;
        std::int32_t                       minX = 0, minZ = 0;     ///< block coordinates of the section's min corner
        glm::vec3                          bmin{0.0f}, bmax{0.0f}; ///< section-local AABB of all geometry
        std::vector<Part>                  opaque, transparent;
        std::size_t                        bytes = 0; ///< estimate: vertices * 24
        std::shared_ptr<const MeshBuffers> source;    ///< retained DH buffers (for re-conversion)
        Section();
        Section(Section&&) noexcept;
        Section& operator=(Section&&) noexcept;
        ~Section();
    };

    struct Stats {
        std::size_t pending      = 0;
        std::size_t liveSections = 0;
        std::size_t liveBytes    = 0;
        std::size_t liveVertices = 0;
        std::size_t uploadsTotal = 0;
        std::size_t staleRemoves = 0; ///< removeIf calls ignored because a newer upload existed
    };

    MeshUploader();
    ~MeshUploader() override;

    // IVertexSink
    void     upload(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) override;
    void     remove(std::uint64_t sectionPos) override;
    UploadId uploadWithId(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) override;
    void     removeIf(std::uint64_t sectionPos, UploadId id) override;

    void         setSettings(MeshSettings const& s);
    MeshSettings settings() const;

    /// Render thread. Processes up to `maxUploads` queued uploads (removes are free). Returns uploads done.
    std::size_t pump(ScreenContext& ctx, std::size_t maxUploads);

    /// Drops every section and queued op; takes effect inside the next pump() (render thread).
    void requestClear();

    std::unordered_map<std::uint64_t, Section> const& sections() const { return sections_; }
    Stats                                             stats() const;
    /// Packed-light statistics of the last converted section (diagnostics): min/max of StageAVertex::v and the
    /// fraction of vertices with v == 0.
    struct LightStats {
        float vMin = 0, vMax = 0, zeroFrac = 0;
        bool  valid = false;
    };
    LightStats lightStats() const;

    /// Packed colour order discovered from the Tessellator (valid after the first successful pump()).
    bool formatReady() const { return formatReady_; }

private:
    struct Converted {
        std::vector<StageAVertex> opaque, transparent;
    };
    struct Op {
        bool                               isRemove = false;
        std::uint64_t                      pos      = 0;
        std::shared_ptr<const MeshBuffers> source;
        Converted                          conv;
        bool                               converted = false;
        std::uint32_t                      gen       = 0;
        UploadCallback                     onDone;
    };
    struct Format; // colour packing + MeshData templates (defined in the .cpp)

    UploadCallback removeLocked(std::uint64_t sectionPos);
    void           convertInto(MeshBuffers const& b, std::uint64_t pos, MeshSettings const& s, Converted& out) const;
    bool           ensureFormat(ScreenContext& ctx);
    bool
    applyUpload(Op& op, MeshSettings const& s, std::uint32_t curGen, std::shared_ptr<mce::BufferResourceService>& svc);
    void destroyAll(bool leak);

    mutable std::mutex                    mutex_;
    MeshSettings                          settings_;
    std::uint32_t                         gen_ = 0;
    std::unordered_map<std::uint64_t, Op> latest_;
    /// Id of the newest upload issued per pos (pending or applied); erased by remove()/successful removeIf()/clear.
    /// removeIf compares against this, so it needs no render-thread state.
    std::unordered_map<std::uint64_t, UploadId> newestId_;
    std::atomic<UploadId>                       lastId_{0};
    std::deque<std::uint64_t>                   order_;
    std::atomic<bool>                           rebuildAll_{false};
    std::atomic<bool>                           clearRequested_{false};
    std::atomic<std::size_t>                    pending_{0};

    // render thread only
    std::unordered_map<std::uint64_t, Section> sections_;
    mce::BufferResourceService*                service_      = nullptr;
    bool                                       formatReady_  = false;
    bool                                       formatFailed_ = false;
    std::unique_ptr<Format>                    format_;

    std::atomic<std::size_t> liveSections_{0}, liveBytes_{0}, liveVertices_{0}, uploadsTotal_{0}, staleRemoves_{0};
    std::atomic<float> lsMin_{0}, lsMax_{0}, lsZero_{0};
    std::atomic<bool>  lsValid_{false};
};

} // namespace dh::render
