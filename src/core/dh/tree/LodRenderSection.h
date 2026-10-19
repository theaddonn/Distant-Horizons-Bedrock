#pragma once

// Port of render/QuadTree/LodRenderSection.java: one renderable area of the LodQuadTree.
//
// Lifecycle (Java state is implied by three fields; here the same three observable predicates):
//   not loaded        !gpuUploadInProgress() && !canRender()
//   loading           gpuUploadInProgress()          (build queued/running or upload pending in the sink)
//   renderable        canRender()                     (the sink holds buffers for `pos`)
//   dirty             canRender() && renderDataDirty  (Java `renderDataDirty`: a reload is wanted; the old
//                                                     buffers keep rendering until the new upload lands)
//   enabled           getRenderingEnabled()           (set by the tree tick; what RenderList consumes)
//   disposed          isDisposed()                    (Java close(): buffers freed, never reloaded)
//
// uploadRenderDataToGpuAsync() (Java name kept) queues getAndBuildRenderData on the render-loader executor:
// provider.get(pos) + the four getAdj edge strips -> FullToRender transform -> ColumnMeshBuilder ->
// LodQuadBuilder::build() -> IVertexSink::upload(pos, buffers, cb). The upload callback flips canRender()
// and clears renderDataDirty (LodRenderSection.java:359-367).
//
// Deviations from Java (each noted at the code):
//   * Java removed the queued Runnable from the executor on close() (LodRenderSection.java:490) and
//     cancelled the future; IExecutor has no remove, so each build carries a cancellation token checked
//     before the build starts and before the upload. A cancelled task never uploads.
//   * Java join()ed the GPU upload on the worker thread (line 202) to throttle staging memory; the sink is
//     asynchronous here, so gpuUploadInProgress() stays true until the sink's callback instead.
//   * isAdjacentPosSameDetailLevel read the live player position from Minecraft on the worker thread;
//     here it uses the position of the last tree tick.
//   * Thread safety: Java used `synchronized` methods plus plain fields. Here the state flags are atomics
//     and only the build-token hand-off takes a mutex, so the render thread (RenderList) never blocks on
//     a worker.
//
// Memory: sections are owned by the tree through shared_ptr; queued build tasks hold a weak_ptr (a section
// removed before its load runs is simply skipped), the sink callback holds a strong reference until the
// sink answers (the IVertexSink contract guarantees every upload is answered). GPU buffers are freed through
// IVertexSink::removeIf with the section's own upload id (Java closed its own LodBufferContainer object), so a
// late callback of a disposed section never wipes the replacement section's buffers at the same pos.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "dh/fulldata/FullDataSource.h"
#include "dh/mesh/ColumnMeshBuilder.h"
#include "dh/mesh/LodQuadBuilder.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/platform/IVertexSink.h"
#include "dh/pos/BlockPos.h"
#include "dh/pos/Direction.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/ColumnRenderSource.h"
#include "dh/render/Enums.h"
#include "dh/render/FullToRender.h"
#include "dh/storage/SourceProvider.h"

#include <glm/glm.hpp>

namespace dh::tree {

//===================//
// shared tree state //
//===================//

/// Port of render/CameraZoom.java (the pure parts). A zoomed camera makes LODs in its cone appear closer;
/// the tree uses the magnified distance for them.
struct CameraZoom {
    static constexpr double NOT_ZOOMED_MAGNIFICATION     = 1.0;
    static constexpr double MIN_ZOOM_MAGNIFICATION       = 1.5;
    static constexpr double ZOOM_CONE_PADDING_MULTIPLIER = 1.5;

    /// how many times larger objects appear on screen compared to the player's FOV setting
    double magnification = NOT_ZOOMED_MAGNIFICATION;
    /// tangent of half the zoom cone's horizontal angle (slightly wider than the camera's FOV)
    double coneTanHalfAngle = 0.0;
    /// the camera's look direction projected onto the XZ plane, normalized
    double lookDirectionX = 0.0;
    double lookDirectionZ = 0.0;

    static CameraZoom notZoomed() { return CameraZoom{}; }
    bool              isZoomed() const { return magnification > NOT_ZOOMED_MAGNIFICATION; }

    /// CameraZoom.coneIntersectsCircle: true if any part of the circle is visible through the zoomed camera
    /// (2D, XZ plane).
    bool coneIntersectsCircle(
        double coneOriginX,
        double coneOriginZ,
        double circleCenterX,
        double circleCenterZ,
        double circleRadius
    ) const;

    /// CameraZoom.update() without the wrapper lookups. `projection` is the MC projection (or, when that is
    /// the identity, the pre-multiplied model-view) in the glm layout used by Frustum.h; `lookAt` is
    /// MC_RENDER.getLookAtVector(); `fovSettingDegrees` MC_RENDER.getFovSetting().
    static CameraZoom fromProjection(
        const glm::mat4&  projection,
        const glm::vec3&  lookAt,
        double            fovSettingDegrees,
        HorizontalQuality horizontalQuality,
        int               maxZoomQualityIncrease,
        bool              increaseQualityWhenZoomedIn
    );
};

/// LodQuadTree.updateDetailLevelVariables + calcDetailLevelFromDistance + calcExpectedDetailLevel as a value
/// type (also what the `treemath` golden suite replays). All detail levels are ABSOLUTE (0 = block); add
/// SECTION_MINIMUM_DETAIL_LEVEL for section detail levels.
struct DetailLevelMath {
    /// FullDataUpdatePropagatorV2.HIGHEST_DOWNSAMPLE_DETAIL_LEVEL: sections at/above never zoom.
    static constexpr std::int8_t kHighestDownsampleDetailLevel = sec::SECTION_MINIMUM_DETAIL_LEVEL + 6;

    /// used to calculate when a detail drop will occur: horizontalQuality.distanceUnitInBlocks * 16
    double detailDropOffDistanceUnit = 0.0;
    /// ln(horizontalQuality.quadraticBase)
    double detailDropOffLogBase = 0.0;
    /// smallest numerical detail level that can be rendered (maxHorizontalResolution.detailLevel)
    std::int8_t maxLeafRenderDetailLevel = 0;
    /// largest numerical detail level that can be rendered
    std::int8_t minRootRenderDetailLevel = 0;
    /// the tree's root detail level (section detail)
    std::int8_t treeRootDetailLevel = 0;

    /// LodQuadTree.java:1244-1263.
    static DetailLevelMath compute(
        HorizontalQuality       horizontalQuality,
        MaxHorizontalResolution maxHorizontalResolution,
        std::int32_t            blockRenderDistanceDiameter,
        std::int8_t             treeRootDetailLevel
    );

    /// LodQuadTree.java:1265-1271: clamp(maxDetail, (int)(ln(d / unit) / logBase), ROOT_SECTION_DETAIL_LEVEL).
    std::int8_t calcDetailLevelFromDistance(double blockDistance) const {
        return calcDetailLevelFromDistance(blockDistance, maxLeafRenderDetailLevel);
    }
    std::int8_t calcDetailLevelFromDistance(double blockDistance, std::int8_t maxDetailLevel) const;

    /// LodQuadTree.java:1209-1242.
    std::int8_t calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos, const CameraZoom& zoom) const;
    std::int8_t calcExpectedDetailLevel(
        BlockPos2D        playerPos,
        std::int32_t      targetBlockPosX,
        std::int32_t      targetBlockPosZ,
        const CameraZoom& zoom
    ) const;

private:
    std::int8_t calcExpectedDetailLevel(
        BlockPos2D        playerPos,
        sec::Pos          targetSectionPos,
        std::int32_t      targetBlockPosX,
        std::int32_t      targetBlockPosZ,
        double            targetBlockRadius,
        const CameraZoom& zoom
    ) const;
};

/// The tree state a render section reads from worker threads (Java: `this.quadTree.calcExpectedDetailLevel`
/// + `MC.getPlayerBlockPos()`). Owned by the tree through shared_ptr and shared with every section, so a
/// section that outlives its tree (late sink callback, test holding a reference) never dereferences a dead
/// tree. The tree writes under the mutex once per tick; readers take a snapshot.
class TreeShared {
public:
    void set(const DetailLevelMath& math, const CameraZoom& zoom) {
        std::lock_guard lock(mutex_);
        math_ = math;
        zoom_ = zoom;
    }
    void setZoom(const CameraZoom& zoom) {
        std::lock_guard lock(mutex_);
        zoom_ = zoom;
    }
    void setLastPlayerPos(BlockPos2D p) { lastPlayerPos_.store(pack(p), std::memory_order_release); }

    DetailLevelMath math() const {
        std::lock_guard lock(mutex_);
        return math_;
    }
    CameraZoom zoom() const {
        std::lock_guard lock(mutex_);
        return zoom_;
    }
    void snapshot(DetailLevelMath& math, CameraZoom& zoom) const {
        std::lock_guard lock(mutex_);
        math = math_;
        zoom = zoom_;
    }
    BlockPos2D lastPlayerPos() const { return unpack(lastPlayerPos_.load(std::memory_order_acquire)); }

    std::int8_t calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos) const {
        DetailLevelMath m;
        CameraZoom      z;
        snapshot(m, z);
        return m.calcExpectedDetailLevel(playerPos, sectionPos, z);
    }
    std::int8_t calcExpectedDetailLevel(BlockPos2D playerPos, std::int32_t blockX, std::int32_t blockZ) const {
        DetailLevelMath m;
        CameraZoom      z;
        snapshot(m, z);
        return m.calcExpectedDetailLevel(playerPos, blockX, blockZ, z);
    }

    /// BlockPos2D <-> one int64 (x high 32 bits, z low 32 bits) for lock-free publication.
    static std::int64_t pack(BlockPos2D p) {
        return static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) << 32) | static_cast<std::uint32_t>(p.z)
        );
    }
    static BlockPos2D unpack(std::int64_t v) {
        return BlockPos2D(
            static_cast<std::int32_t>(static_cast<std::uint64_t>(v) >> 32),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)))
        );
    }

private:
    mutable std::mutex        mutex_;
    DetailLevelMath           math_;
    CameraZoom                zoom_;
    std::atomic<std::int64_t> lastPlayerPos_{0};
};

/// The two provider calls a render section makes (Java FullDataSourceProviderV2.get / getAdjForDirection).
/// Abstract so tests can feed synthetic sections without a store + codec; ProviderSectionSource is the
/// production adapter. Both return read-only snapshots (never mutate / cast away const).
struct ISectionDataSource {
    virtual ~ISectionDataSource() = default;

    /// Java get(pos): the section, or an empty one when nothing is stored; nullptr = unavailable
    /// (provider shut down / unreadable record).
    virtual std::shared_ptr<const FullDataSource> get(sec::Pos pos) = 0;

    /// Java getAdjForDirection(pos, dir): only the edge strip of `pos` that faces `dir`; nullptr on error.
    virtual std::shared_ptr<const FullDataSource> getAdj(sec::Pos pos, Direction dir) = 0;
};

class ProviderSectionSource final : public ISectionDataSource {
public:
    explicit ProviderSectionSource(SourceProvider& provider) : provider_(provider) {}

    std::shared_ptr<const FullDataSource> get(sec::Pos pos) override { return provider_.get(pos); }
    std::shared_ptr<const FullDataSource> getAdj(sec::Pos pos, Direction dir) override {
        Result<std::shared_ptr<FullDataSource>> r = provider_.getAdj(pos, dir);
        if (!r) return nullptr;
        return std::shared_ptr<const FullDataSource>(std::move(*r));
    }

private:
    SourceProvider& provider_;
};

/// Everything a section needs besides its tree; shared by all sections of one tree and owned by whoever
/// owns the tree (must outlive it). Java read these from singletons / Config.
struct RenderSectionContext {
    ISectionDataSource* data         = nullptr;
    IClientLevel*       level        = nullptr;
    IBlockRegistry*     registry     = nullptr;
    IVertexSink*        sink         = nullptr;
    IExecutor*          renderLoader = nullptr; ///< ThreadPoolUtil.getRenderLoadingExecutor()

    /// FullDataToRenderDataTransformer config (verticalQuality, culling, block sets).
    TransformOptions transform;
    /// ColumnRenderBufferBuilder / LodQuadBuilder config; `mesh.quad.transparency` is Java's
    /// LodQuadBuilder.getBuilder(enableTransparency = Quality.transparency == COMPLETE).
    MeshOptions mesh;
};

class LodRenderSection : public std::enable_shared_from_this<LodRenderSection> {
public:
    const sec::Pos pos;

    /// Used when a node goes out of render distance but stays in the tree: act as if it were removed for
    /// render-data caching purposes (reload when it comes back), but not for re-creating missing nodes.
    std::atomic<bool> renderDataDirty{false};
    std::atomic<bool> queuedMissingSectionsForRetrieval{false};

    LodRenderSection(sec::Pos pos, std::shared_ptr<const TreeShared> tree, RenderSectionContext& ctx);
    ~LodRenderSection();
    LodRenderSection(const LodRenderSection&)            = delete;
    LodRenderSection& operator=(const LodRenderSection&) = delete;

    //======================================//
    // render data generation and uploading //
    //======================================//

    /// Queues load + build + upload on the render-loader executor.
    /// @return true if the upload started, false if it wasn't able to for any reason (already in progress,
    ///         executor rejected the task, section disposed).
    bool uploadRenderDataToGpuAsync();

    //=================//
    // rendering state //
    //=================//

    /// Java `renderBufferContainer != null`: the sink holds (possibly empty) geometry for this section.
    bool canRender() const { return hasGpuBuffers_.load(std::memory_order_acquire); }
    /// canRender() && !renderDataDirty: dirty sections trigger new uploads even though data is present.
    bool gpuUploadComplete() const { return canRender() && !renderDataDirty.load(std::memory_order_acquire); }
    bool gpuUploadInProgress() const { return buildInProgress_.load(std::memory_order_acquire); }

    bool getRenderingEnabled() const { return renderingEnabled_.load(std::memory_order_acquire); }
    void setRenderingEnabled(bool enabled) { renderingEnabled_.store(enabled, std::memory_order_release); }

    /// Whether the last successful upload carried opaque / transparent buffers (LodBufferContainer vbo lists).
    bool hasOpaque() const { return hasOpaque_.load(std::memory_order_acquire); }
    bool hasTransparent() const { return hasTransparent_.load(std::memory_order_acquire); }
    /// Diagnostic: the render data of the last build had sky 0 / block 0 on the top datapoint of every column
    /// (the section renders near-black). false until a build ran.
    bool renderDark() const { return renderDark_.load(std::memory_order_acquire); }

    //==============//
    // base methods //
    //==============//

    /// Java close(): cancels a queued/running load, disables rendering and frees the GPU buffers
    /// (IVertexSink::remove). Idempotent. A disposed section never loads again.
    void dispose();
    bool isDisposed() const { return disposed_.load(std::memory_order_acquire); }

    /// "pos=[6*1,-3] enabled=[true] canRender=[false] uploading=[false] "
    std::string toString() const;

private:
    struct BuildToken {
        std::atomic<bool> cancelled{false};
    };

    /// Build the quads for this section (worker thread). nullopt = nothing can be uploaded (Java returned
    /// null: no data, transform failed, or the builder threw).
    std::optional<LodQuadBuilder> getAndBuildRenderData();
    /// Java getRenderSourceForPos(pos, direction): nullopt where Java returned null.
    std::optional<ColumnRenderSource> renderSourceForPos(sec::Pos pos, std::optional<Direction> direction);
    bool                              isAdjacentPosSameDetailLevel(Direction direction) const;

    void runBuild(const std::shared_ptr<BuildToken>& token);
    void onUploadDone(
        bool                               ok,
        bool                               hasOpaque,
        bool                               hasTransparent,
        const std::shared_ptr<BuildToken>& token,
        IVertexSink::UploadId              uploadId
    );
    /// Java `future.handle(...)`: forget the build if `token` is still the current one.
    void finishBuild(const std::shared_ptr<BuildToken>& token);

    std::shared_ptr<const TreeShared> tree_;
    RenderSectionContext&             ctx_;

    std::mutex                  buildMutex_; ///< guards currentBuild_
    std::shared_ptr<BuildToken> currentBuild_;

    /// Identity of the newest upload this section issued (IVertexSink::uploadWithId); what dispose() and a
    /// late callback pass to removeIf so a replacement section's buffers at the same pos survive.
    std::atomic<IVertexSink::UploadId> lastUploadId_{0};

    std::atomic<bool> buildInProgress_{false};
    std::atomic<bool> hasGpuBuffers_{false};
    std::atomic<bool> hasOpaque_{false};
    std::atomic<bool> hasTransparent_{false};
    std::atomic<bool> renderDark_{false};
    std::atomic<bool> renderingEnabled_{false};
    std::atomic<bool> disposed_{false};
};

} // namespace dh::tree
