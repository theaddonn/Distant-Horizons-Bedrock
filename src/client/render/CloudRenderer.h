#pragma once

// Distant-Horizons-style clouds: one boxed cloud layer built from the vanilla clouds.png pattern, covering the LOD far
// distance around the camera, drawn with DH_TerrainTransparent after the LOD terrain.
//
// The layer is split into stationary square tiles in "cloud space" (the pattern's own frame, kTileCells cells =
// kTileBlocks blocks per side), each tile into one quad buffer per face group (down, up, north, south, west, east).
// Every non-empty (tile, group) is a MeshUploader section with the reserved detail level 0 (no real section uses it)
// and pos = encode(0, ti * 8 + group, tj), so it goes through the normal mce::Mesh upload / settings re-conversion
// path. Tiles are built on one std::async worker (a batch of the nearest missing tiles per job); camera movement only
// adds tiles entering the radius and releases tiles leaving it. The drift is NOT baked in: it is the world-matrix
// translate (world = cloud space + offset, offset.x = -speed * t). The mesh is white (face shading only); the sky
// tint is a per-draw parameter (u_model[0][3][0], decoded by the DH_Terrain VS), so day/night changes are continuous.
//
// draw() culls per tile (fully faded beyond the fog end, frustum) and, while the VS drops back faces (marker 4),
// per face group the VS would drop entirely, so only a fraction of the layer is submitted.
//
// Threading: update()/draw() render thread only. Tile building runs on a std::async worker (waited in the dtor).

#include "client/render/CloudMesh.h"
#include "client/render/Frustum5.h"
#include "client/render/MeshUploader.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
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
    static constexpr int   kTileBlocks     = cloud::kTileCells * kCellBlocks;
    static constexpr int   kTilesPerJob    = 8;

    struct Inputs {
        bool       enable = false;
        glm::dvec3 camera{0.0};
        float      farBlocks = 0.0f; ///< LOD far distance (blocks): coverage radius and fog end
        float      cloudY    = 192.0f;
        float      minY      = -64.0f; ///< MeshSettings::yOffsetBlocks the uploader applies to y
        glm::vec3  tint{1.0f};         ///< 0..1 per channel, applied per draw (8 bits each), no rebuild
    };

    struct Stats {
        std::size_t quads           = 0; ///< quads of all live (drawable) tiles
        std::size_t tiles           = 0; ///< live (drawable) tiles
        std::size_t tilesDrawn      = 0; ///< last draw()
        std::size_t drawCalls       = 0; ///< last draw()
        std::size_t quadsDrawn      = 0; ///< last draw()
        double      lastBuildMs     = 0; ///< last tile batch
        std::size_t builds          = 0; ///< tiles built
        bool        drawn           = false;
        bool        fallbackPattern = false;
    };

    explicit CloudRenderer(MeshUploader& sink);
    ~CloudRenderer();
    CloudRenderer(CloudRenderer const&)            = delete;
    CloudRenderer& operator=(CloudRenderer const&) = delete;

    /// Render thread, after MeshUploader::pump(): promotes finished uploads, drops lost tiles and starts a tile batch
    /// when needed. Returns true when at least one tile is ready to draw.
    bool update(Inputs const& in);

    /// Render thread. Draws the ready tiles (camera-relative translate per tile) with `material`
    /// (DH_TerrainTransparent), skipping tiles beyond `fogEnd` or outside `frustum` (not tested when !valid) and, with
    /// the back-face marker, face groups the VS would drop anyway. The caller has already set the projection (far
    /// plane push). Returns the number of draw calls.
    std::size_t draw(
        mce::MeshContext&       mctx,
        mce::MaterialPtr const& material,
        glm::dvec3 const&       camera,
        bool                    cameraRelative,
        float                   fogStart,
        float                   fogEnd,
        Frustum5 const&         frustum
    );

    /// Bytes / section count of the live cloud sections (0 if none); LodRenderer excludes them from the section
    /// statistics.
    std::size_t liveBytes() const;
    std::size_t liveSectionCount() const;

    /// Removes every tile's sections (a running batch is dropped when it finishes).
    void release();

    Stats const& stats() const { return stats_; }

    /// True when draw() asks the VS to drop back faces (cloud marker 4): camera-relative positions and the camera
    /// outside the slab [slabBottom(), slabBottom() + kThickness].
    bool cullsBackFaces(double cameraY, bool cameraRelative) const;

    /// World y of the bottom of the drawn slab: the live tiles' own (rounded) bottom plus the uploader's min-y offset,
    /// so a pending rebuild at a new height cannot skew the inside-slab test (192 before the first mesh).
    float slabBottom() const {
        return haveY_ ? static_cast<float>(liveY_) + minY_ : 192.0f;
    }

    /// Numpad 6 A/B switch for the back-face cull (default on). Render thread only (key handler and draw()).
    static bool backFaceCullEnabled();
    static void toggleBackFaceCull();

    /// pos of a cloud section (detail level 0 never occurs for real sections: the minimum is 6).
    static bool isCloudSection(std::uint64_t pos);

private:
    struct Group {
        std::uint64_t                     pos = 0;
        MeshUploader::UploadId            id  = 0;
        std::shared_ptr<std::atomic<int>> state; ///< 0 pending, 1 renderable, 2 dropped
        std::size_t                       quads = 0; ///< 0 = no section for this group
    };
    struct Tile {
        int                                   ti = 0, tj = 0, yBottom = 0;
        std::array<Group, cloud::kFaceGroups> g;
        std::size_t                           quads = 0;
        bool                                  ready = false;
        std::chrono::steady_clock::time_point since{};
    };
    struct Batch {
        std::vector<Tile> tiles;
        double            ms     = 0;
        bool              failed = false;
    };

    void   loadPattern();
    double offsetX() const;
    void   dropTile(Tile const& t);

    MeshUploader&                         sink_;
    std::shared_ptr<const cloud::Pattern> pattern_;
    bool                                  patternTried_ = false;
    std::chrono::steady_clock::time_point t0_;
    glm::vec3                             tint_{1.0f}; ///< latest Inputs::tint, applied per draw
    float                                 minY_ = -64.0f; ///< latest Inputs::minY (the uploader's y offset)
    std::chrono::steady_clock::time_point retryAfter_{};

    std::unordered_map<std::uint64_t, Tile> tiles_; ///< key: (ti, tj) packed
    bool                                    haveY_ = false;
    int                                     liveY_ = 0; ///< yBottom of the ready tiles
    std::future<Batch>                      job_;
    bool                                    jobRunning_ = false;
    double                                  totalBuildMs_ = 0;
    Stats                                   stats_;
    bool                                    loggedFirst_ = false;
};

} // namespace dh::render
