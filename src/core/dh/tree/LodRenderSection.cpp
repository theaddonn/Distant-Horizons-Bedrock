#include "dh/tree/LodRenderSection.h"

#include <utility>

#include "dh/platform/ILogger.h"
#include "dh/tree/LodQuadTree.h"

namespace dh::tree {

LodRenderSection::LodRenderSection(sec::Pos pos_, LodQuadTree& quadTree, RenderSectionContext& ctx)
: pos(pos_),
  quadTree_(quadTree),
  ctx_(ctx) {}

LodRenderSection::~LodRenderSection() { dispose(); }

//======================================//
// render data generation and uploading //
//======================================//

bool LodRenderSection::uploadRenderDataToGpuAsync() {
    if (isDisposed() || ctx_.renderLoader == nullptr || ctx_.sink == nullptr || ctx_.data == nullptr) {
        return false;
    }

    std::shared_ptr<BuildToken> token;
    {
        std::lock_guard lock(buildMutex_);
        if (currentBuild_) {
            // don't accidentally queue multiple uploads at the same time
            return false;
        }
        token         = std::make_shared<BuildToken>();
        currentBuild_ = token;
        buildInProgress_.store(true, std::memory_order_release);
    }

    // The task only holds a weak reference: a section dropped from the tree before its load runs is skipped
    // (Java removed the Runnable from the executor instead, LodRenderSection.java:490).
    std::weak_ptr<LodRenderSection> weak   = weak_from_this();
    const bool                      queued = ctx_.renderLoader->execute([weak, token]() {
        std::shared_ptr<LodRenderSection> self = weak.lock();
        if (!self) {
            return;
        }
        self->runBuild(token);
    });
    if (!queued) {
        // RejectedExecutionException: the pool is probably being resized, the next tick retries
        finishBuild(token);
        return false;
    }
    return true;
}

void LodRenderSection::runBuild(const std::shared_ptr<BuildToken>& token) {
    if (token->cancelled.load(std::memory_order_acquire) || isDisposed()) {
        finishBuild(token);
        return;
    }

    std::optional<LodQuadBuilder> lodQuadBuilder = getAndBuildRenderData();
    if (!lodQuadBuilder.has_value()) {
        // nothing needs to be rendered (Java: future.complete(null))
        finishBuild(token);
        return;
    }

    MeshBuffers buffers        = lodQuadBuilder->build();
    const bool  hasOpaque      = !buffers.opaque.empty();
    const bool  hasTransparent = !buffers.transparent.empty();

    if (token->cancelled.load(std::memory_order_acquire)) {
        // closed while building: Java uploaded anyway and closed the buffer afterwards (line 370-377);
        // skipping the upload has the same observable result without the GPU round trip
        finishBuild(token);
        return;
    }

    // uploading happens on the sink's thread; the callback may run inline
    std::shared_ptr<LodRenderSection> self = shared_from_this();
    ctx_.sink->upload(pos, std::move(buffers), [self, token, hasOpaque, hasTransparent](bool ok) {
        self->onUploadDone(ok, hasOpaque, hasTransparent, token);
    });
}

void LodRenderSection::onUploadDone(
    bool                               ok,
    bool                               hasOpaque,
    bool                               hasTransparent,
    const std::shared_ptr<BuildToken>& token
) {
    if (ok) {
        // LodRenderSection.java:358-367: the new container replaces the old one, upload complete
        hasOpaque_.store(hasOpaque, std::memory_order_release);
        hasTransparent_.store(hasTransparent, std::memory_order_release);
        hasGpuBuffers_.store(true, std::memory_order_release);
        renderDataDirty.store(false, std::memory_order_release);

        if (token->cancelled.load(std::memory_order_acquire) || isDisposed()) {
            // the section was closed before this point: this buffer would become homeless (Java :370-377)
            hasGpuBuffers_.store(false, std::memory_order_release);
            ctx_.sink->remove(pos);
        }
    } else {
        // Java: logged unless it was a shutdown exception; the old container (if any) keeps rendering
        if (!isDisposed()) {
            log::logf(LogLevel::Debug, "render upload dropped for pos [%s]", sec::toString(pos).c_str());
        }
    }
    finishBuild(token);
}

void LodRenderSection::finishBuild(const std::shared_ptr<BuildToken>& token) {
    std::lock_guard lock(buildMutex_);
    if (currentBuild_ == token) {
        currentBuild_.reset();
        buildInProgress_.store(false, std::memory_order_release);
    }
}

//=======================//
// Get LOD ID data       //
// and build render data //
//=======================//

std::optional<LodQuadBuilder> LodRenderSection::getAndBuildRenderData() {
    std::optional<ColumnRenderSource> thisRenderSource = renderSourceForPos(pos, std::nullopt);
    if (!thisRenderSource.has_value()) {
        // nothing needs to be rendered
        return std::nullopt;
    }
    if (thisRenderSource->isEmpty) {
        // Java: ColumnRenderBufferBuilder.makeLodRenderData threw IllegalArgumentException on the first
        // populateColumnView of an empty source; LodRenderSection caught it (line 278) and returned null.
        return std::nullopt;
    }

    LodQuadBuilder lodQuadBuilder(ctx_.mesh.quad);

    // get the adjacent positions
    std::optional<ColumnRenderSource> adjacent[4];
    AdjacentSources                   adj;
    for (Direction direction : {Direction::North, Direction::South, Direction::East, Direction::West}) {
        const int ci                                 = compassIndex(direction);
        adjacent[ci]                                 = renderSourceForPos(pos, direction);
        adj.bySide[static_cast<std::size_t>(ci)]     = adjacent[ci].has_value() ? &*adjacent[ci] : nullptr;
        adj.sameDetail[static_cast<std::size_t>(ci)] = isAdjacentPosSameDetailLevel(direction);
    }

    if (!makeLodRenderData(lodQuadBuilder, *thisRenderSource, *ctx_.level, adj, ctx_.mesh)) {
        // Java: an exception escaped makeLodRenderData (bad data point), caught at line 278 -> null.
        // (The other `false` reason, a finer adjacent source, cannot happen: getAdj is same-detail by construction.)
        log::logf(
            LogLevel::Error,
            "Unexpected error while loading LodRenderSection [%s] adjacent data.",
            sec::toString(pos).c_str()
        );
        return std::nullopt;
    }
    return lodQuadBuilder;
}

std::optional<ColumnRenderSource>
LodRenderSection::renderSourceForPos(sec::Pos sectionPos, std::optional<Direction> direction) {
    if (direction.has_value()) {
        sectionPos = sec::adjacent(sectionPos, *direction);
    }

    // no direction means get the center LOD; adjacent sections only need the strip facing us
    const std::shared_ptr<const FullDataSource> fullDataSource =
        direction.has_value() ? ctx_.data->getAdj(sectionPos, opposite(*direction)) : ctx_.data->get(sectionPos);
    if (!fullDataSource) {
        return std::nullopt;
    }
    return transformFullDataToRenderSource(*fullDataSource, *ctx_.level, *ctx_.registry, ctx_.transform);
}

bool LodRenderSection::isAdjacentPosSameDetailLevel(Direction direction) const {
    const sec::Pos adjPos      = sec::adjacent(pos, direction);
    std::int8_t    detailLevel = quadTree_.calcExpectedDetailLevel(quadTree_.lastTickPlayerPos(), adjPos);
    detailLevel                = static_cast<std::int8_t>(detailLevel + sec::SECTION_MINIMUM_DETAIL_LEVEL);
    return detailLevel == sec::detail(pos);
}

//==============//
// base methods //
//==============//

void LodRenderSection::dispose() {
    if (disposed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    // render loading is no longer needed: cancel the task instead of Java's executor.remove(runnable)
    std::shared_ptr<BuildToken> token;
    {
        std::lock_guard lock(buildMutex_);
        token = std::move(currentBuild_);
        currentBuild_.reset();
        buildInProgress_.store(false, std::memory_order_release);
    }
    if (token) {
        token->cancelled.store(true, std::memory_order_release);
    }

    setRenderingEnabled(false);
    if (hasGpuBuffers_.exchange(false, std::memory_order_acq_rel) && ctx_.sink != nullptr) {
        ctx_.sink->remove(pos);
    }
}

std::string LodRenderSection::toString() const {
    return "pos=[" + sec::toString(pos) + "] " + "enabled=[" + (getRenderingEnabled() ? "true" : "false") + "] "
         + "canRender=[" + (canRender() ? "true" : "false") + "] " + "uploading=["
         + (gpuUploadInProgress() ? "true" : "false") + "] ";
}

} // namespace dh::tree
