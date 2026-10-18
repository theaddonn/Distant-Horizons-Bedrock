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
// sink answers (the IVertexSink contract guarantees every upload is answered).

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
#include "dh/pos/Direction.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/ColumnRenderSource.h"
#include "dh/render/FullToRender.h"
#include "dh/storage/SourceProvider.h"

namespace dh::tree {

class LodQuadTree;

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

    LodRenderSection(sec::Pos pos, LodQuadTree& quadTree, RenderSectionContext& ctx);
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
    void onUploadDone(bool ok, bool hasOpaque, bool hasTransparent, const std::shared_ptr<BuildToken>& token);
    /// Java `future.handle(...)`: forget the build if `token` is still the current one.
    void finishBuild(const std::shared_ptr<BuildToken>& token);

    LodQuadTree&          quadTree_;
    RenderSectionContext& ctx_;

    std::mutex                  buildMutex_; ///< guards currentBuild_
    std::shared_ptr<BuildToken> currentBuild_;

    std::atomic<bool> buildInProgress_{false};
    std::atomic<bool> hasGpuBuffers_{false};
    std::atomic<bool> hasOpaque_{false};
    std::atomic<bool> hasTransparent_{false};
    std::atomic<bool> renderingEnabled_{false};
    std::atomic<bool> disposed_{false};
};

} // namespace dh::tree
