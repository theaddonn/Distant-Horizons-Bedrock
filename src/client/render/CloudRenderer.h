#pragma once

// Distant-Horizons-style clouds: one flat/boxed cloud layer built from the vanilla clouds.png pattern, covering the
// LOD far distance around the camera, drawn with DH_TerrainTransparent after the LOD terrain.
//
// The mesh is built on a worker thread in "cloud space" (the pattern's own frame) around an anchor snapped to the
// camera, and handed to the MeshUploader as a reserved section (detail level 0, which no real section uses) so it
// goes through the normal mce::Mesh upload / settings re-conversion path. The drift is NOT baked into the mesh: it is
// the world-matrix translate (world = cloud space + offset, offset.x = -speed * t), so a rebuild is only needed when
// the camera leaves the covered square (margin kMarginBlocks), the far distance / height changes, or the section
// disappeared (requestClear on world exit). The mesh is white (face shading only); the sky tint is a per-draw
// parameter (u_model[0][3][0], decoded by the DH_Terrain VS), so day/night changes are continuous.
//
// Threading: update()/draw() render thread only. Mesh building runs on a std::async worker (waited in the dtor).

#include "client/render/CloudMesh.h"
#include "client/render/MeshUploader.h"

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace mce {
class MaterialPtr;
class MeshContext;
} // namespace mce

namespace dh::render {

class CloudRenderer {
public:
    /// Texel size (blocks) and drift of the vanilla clouds (Java: 12 blocks per texel, 0.03 blocks/tick = 0.6 b/s
    /// towards -X). Bedrock's own values are unverified in-game (docs/spikes/m1-lods.md, Run 10).
    static constexpr int   kCellBlocks     = 12;
    static constexpr float kDriftBlocksSec = 0.6f;
    static constexpr float kThickness      = 4.0f;
    static constexpr float kAlpha          = 0.8f;
    static constexpr int   kMarginBlocks   = 384;

    struct Inputs {
        bool       enable = false;
        glm::dvec3 camera{0.0};
        float      farBlocks = 0.0f; ///< LOD far distance (blocks): coverage radius and fog end
        float      cloudY    = 192.0f;
        float      minY      = -64.0f; ///< MeshSettings::yOffsetBlocks the uploader applies to y
        glm::vec3  tint{1.0f};         ///< 0..1 per channel, applied per draw (8 bits each), no rebuild
    };

    struct Stats {
        std::size_t quads           = 0;
        double      lastBuildMs     = 0;
        std::size_t builds          = 0;
        bool        drawn           = false;
        bool        fallbackPattern = false;
    };

    explicit CloudRenderer(MeshUploader& sink);
    ~CloudRenderer();
    CloudRenderer(CloudRenderer const&)            = delete;
    CloudRenderer& operator=(CloudRenderer const&) = delete;

    /// Render thread, after MeshUploader::pump(): promotes finished uploads and starts a rebuild when needed. Returns
    /// true when a cloud section is ready to draw.
    bool update(Inputs const& in);

    /// Render thread. Draws the current section (camera-relative) with `material` (DH_TerrainTransparent). The caller
    /// has already set the projection (far plane push). Returns the number of draw calls.
    std::size_t draw(
        mce::MeshContext&       mctx,
        mce::MaterialPtr const& material,
        glm::dvec3 const&       camera,
        bool                    cameraRelative,
        float                   fogStart,
        float                   fogEnd
    );

    /// Bytes of the live cloud section (0 if none); LodRenderer excludes it from the section statistics.
    std::size_t liveBytes() const;

    /// Removes the section and cancels interest in a running build.
    void release();

    Stats const& stats() const { return stats_; }

    /// True when draw() asks the VS to drop back faces (cloud marker 4): camera-relative positions and the camera
    /// outside the slab [cloudY, cloudY + kThickness] of the latest update().
    bool cullsBackFaces(double cameraY, bool cameraRelative) const;

    /// World y of the slab bottom used by the latest update() (cloud probe, Numpad 5).
    float slabBottom() const { return cloudY_; }

    /// Numpad 6 A/B switch for the back-face cull (default on). Render thread only (key handler and draw()).
    static bool backFaceCullEnabled();
    static void toggleBackFaceCull();

    /// pos of a cloud section (detail level 0 never occurs for real sections: the minimum is 6).
    static bool isCloudSection(std::uint64_t pos);

private:
    struct Key {
        long          anchorX = 0, anchorZ = 0;
        int           half    = 0;
        int           yBottom = 0;
        bool          operator==(Key const& o) const {
            return anchorX == o.anchorX && anchorZ == o.anchorZ && half == o.half && yBottom == o.yBottom;
        }
    };
    struct Built {
        Key                               key;
        std::uint64_t                     pos = 0;
        MeshUploader::UploadId            id  = 0;
        std::shared_ptr<std::atomic<int>> state; ///< 0 pending, 1 renderable, 2 dropped
        std::size_t                       quads = 0;
        double                            ms    = 0;
        bool                              empty = false;
    };

    void   loadPattern();
    double offsetX() const;

    MeshUploader&                         sink_;
    std::shared_ptr<const cloud::Pattern> pattern_;
    bool                                  patternTried_ = false;
    std::chrono::steady_clock::time_point t0_;
    glm::vec3                             tint_{1.0f}; ///< latest Inputs::tint, applied per draw
    float                                 cloudY_ = 192.0f; ///< latest Inputs::cloudY (world y of the slab bottom)
    std::chrono::steady_clock::time_point pendingSince_{};
    std::chrono::steady_clock::time_point retryAfter_{};

    bool               haveCur_ = false;
    Built              cur_;
    bool               havePending_ = false;
    Built              pending_;
    std::future<Built> job_;
    bool               jobRunning_ = false;
    Stats              stats_;
    bool               loggedFirst_ = false;
};

} // namespace dh::render
