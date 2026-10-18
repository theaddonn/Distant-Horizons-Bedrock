#pragma once

// Port of render/QuadTree/LodQuadTree.java (+ the pure parts of render/CameraZoom.java): the client's core
// data structure. A QuadTree<shared_ptr<LodRenderSection>> that is re-centred on the player every tick
// (ClientLevelModule ticks it every 100 ms), decides per node which detail level should render, queues
// loads on the render-loader executor, queues missing data for retrieval (world gen / server download) and
// publishes the list of enabled sections for RenderList.
//
// Threading (as Java): tryTick() runs on one tick thread and skips the tick if the previous one is still
// running (tryLock). collectEnabled() may run on the render thread concurrently (double-buffered list under
// a short lock). queueReload() may be called from any thread (provider update listener, network). Section
// builds run on the render-loader executor; retrieval queuing on `retrievalQueueExecutor` (or inline).
//
// Deviations from Java, each marked at the code:
//   * Beacon rendering (tryRefreshRenderingBeaconsAsync) is not ported (no generic renderer yet).
//   * The retrieval queuing thread's "loop + sleep(100 ms) while work remains" is replaced by one queuing
//     pass per tick (ticks are 100 ms apart, so the cadence is the same).
//   * getPositionsToRetrieve (WP-J2's generated provider) is an injected hook; without it nothing is queued
//     (Java's base provider returned null = "cannot generate").
//   * The regen chunk count estimate (a DB query every 10 s) is not ported; the estimate only counts the
//     missing positions.
//   * close() disposes every section synchronously under the tick lock (Java did it on the cleanup pool).
//   * The config is read through a const Options* at tick time (Java read Config singletons).
//
// Review notes (WP-D3 hand-off): provider update listeners run with the position lock held; the tree's
// listener (attachToProvider) only pushes the pos onto `sectionsToReload` and the tick thread reloads.
// The listener captures a shared_ptr to that queue, never `this`, so a listener invoked after
// removeListener()/destruction is harmless.
// Likewise the retrieval futures' callbacks capture only the shared RetrievalState, and sections only the
// shared TreeShared (detail math, zoom, last player pos), never the tree itself.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "dh/config/Options.h"
#include "dh/gen/RetrievalQueue.h"
#include "dh/platform/IExecutor.h"
#include "dh/pos/BlockPos.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/Enums.h"
#include "dh/storage/SourceProvider.h"
#include "dh/tree/LodRenderSection.h"
#include "dh/tree/QuadTree.h"
#include "dh/tree/TickNodeHolder.h"

namespace dh::tree {

//=============//
// LodQuadTree //
//=============//

/// WP-J2 seam: Java FullDataSourceProviderV2.getPositionsToRetrieve(pos, generatorDetailLevel). nullopt when
/// the provider cannot generate anything; an empty list when everything is already generated.
using PositionsToRetrieveFn =
    std::function<std::optional<std::vector<sec::Pos>>(sec::Pos pos, std::int8_t generatorDetailLevel)>;

/// Optional collaborators of the tree (all may be null / empty).
struct LodQuadTreeDeps {
    /// Java fullDataSourceProvider.queuePositionForRetrieval / removeRetrievalRequestIf target. null = no
    /// retrieval (world gen / download) at all.
    IRetrievalQueue* retrievalQueue = nullptr;
    /// Java "LodQuadTree Data Retrieval Queue" single thread. null = queuing runs inside tryTick.
    IExecutor* retrievalQueueExecutor = nullptr;
    /// ThreadPoolUtil.getFileHandlerExecutor() for threadPoolCanAcceptWorldGenTasks(); null = not checked.
    IExecutor* fileHandler = nullptr;
    /// See PositionsToRetrieveFn.
    PositionsToRetrieveFn positionsToRetrieve;
    /// Java fullDataSourceProvider.canQueueRetrievalNow() (migration idle, queue limits). Empty = true
    /// whenever `retrievalQueue` is set.
    std::function<bool()> canQueueRetrievalNow;
};

class LodQuadTree : public QuadTree<std::shared_ptr<LodRenderSection>> {
public:
    using Section = std::shared_ptr<LodRenderSection>;
    using Base    = QuadTree<Section>;
    using Node    = QuadNode<Section>;

    /// LodQuadTree.java:170: nodes are re-checked after moving FullDataSourceV2.WIDTH blocks.
    static constexpr std::int32_t kBlockDistanceForNodeClearing = lod::FULL_DATA_WIDTH;
    static constexpr std::int8_t  kHighestDownsampleDetailLevel = DetailLevelMath::kHighestDownsampleDetailLevel;
    /// FullDataUpdatePropagatorV2.NUMBER_OF_PARENT_UPDATE_TASKS_PER_THREAD.
    static constexpr std::int32_t kParentUpdateTasksPerThread = 10;

    /// ClientRenderState.getViewDiameterInBlocks(): lodChunkRenderDistanceRadius * 16 * 2.
    static std::int32_t viewDiameterInBlocks(const Options& options);
    /// QuadTree's root detail level for a diameter with the section leaf level (max(max(1, 6), log2(diameter))).
    static std::int8_t rootDetailLevelFor(std::int32_t viewDiameterInBlocks);

    /// @param options read at every tick (must outlive the tree; Java read Config)
    /// @param ctx shared by every section (must outlive the tree)
    LodQuadTree(
        const Options&        options,
        std::int32_t          viewDiameterInBlocks,
        BlockPos2D            initialPlayerBlockPos,
        RenderSectionContext& ctx,
        LodQuadTreeDeps       deps = {}
    );
    ~LodQuadTree();

    const std::int32_t blockRenderDistanceDiameter;

    //==================//
    // property getters //
    //==================//

    /// populateListWithEnabledRenderSections: the sections that should be rendered (last completed tick).
    void collectEnabled(std::vector<Section>& out) const;

    /// The player position of the last tick (what Java's worker threads read live from Minecraft).
    BlockPos2D lastTickPlayerPos() const { return shared_->lastPlayerPos(); }
    /// getCenterBlockPos() as published by the last tick, readable from any thread (RenderList's sort centre;
    /// QuadTree::getCenterBlockPos itself is only safe on the tick thread).
    BlockPos2D renderCenterBlockPos() const { return TreeShared::unpack(centerBlockPosAtomic_.load()); }

    DetailLevelMath detailLevelMath() const { return shared_->math(); }
    CameraZoom      cameraZoom() const { return shared_->zoom(); }
    /// Zoom magnification hook (Java CameraZoom.INSTANCE.update(renderState) every frame).
    void setCameraZoom(const CameraZoom& zoom) { shared_->setZoom(zoom); }

    const RenderSectionContext&       context() const { return ctx_; }
    std::shared_ptr<const TreeShared> shared() const { return shared_; }

    //=============//
    // tick update //
    //=============//

    /// Update the tree using the player position and queue any necessary work. Does nothing if a tick is
    /// still running (Java treeTickLock.tryLock()).
    void tryTick(BlockPos2D playerPos);

    //====================//
    // detail level logic //
    //====================//

    /// Expected ABSOLUTE detail level of the section (uses the current detail math + camera zoom).
    std::int8_t calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos) const {
        return shared_->calcExpectedDetailLevel(playerPos, sectionPos);
    }
    std::int8_t
    calcExpectedDetailLevel(BlockPos2D playerPos, std::int32_t targetBlockPosX, std::int32_t targetBlockPosZ) const {
        return shared_->calcExpectedDetailLevel(playerPos, targetBlockPosX, targetBlockPosZ);
    }

    //==========================//
    // external render requests //
    //==========================//

    /// Marks every section for reloading (clearExistingData: hard reload, sections are closed).
    void clearRenderDataCache(bool clearExistingData);

    /// queuePosToReload: refresh a section (and its 4 neighbours, for lighting / water at borders) on the
    /// next tick. Thread safe; only enqueues (safe inside a provider update listener).
    void queueReload(sec::Pos pos);

    /// Registers queueReload as an update listener of `provider`; removed again by close(). The listener does
    /// not reference the tree, so an update racing with removal is harmless.
    void attachToProvider(SourceProvider& provider);

    /// LodQuadTree.onConfigValueSet (Config.Common.WorldGenerator.generatorPlan listener).
    void onGeneratorPlanChanged(bool generationEnabled);

    //===========//
    // retrieval //
    //===========//

    std::size_t missingRetrievalCount() const;
    std::size_t queuedRetrievalCount() const;
    /// Java fullDataSourceProvider.setCanRegenerate(...) value (LodQuadTree.java:942-959): false while a
    /// coarser-than-block section is still missing ("don't let any regeneration happen until all low-detail
    /// LODs have finished generating"); true when every missing position is block detail or nothing is missing.
    bool canRegenerate() const;
    /// Java setEstimatedRemainingRetrievalChunkCount (missing positions only, see header).
    std::int64_t estimatedRemainingRetrievalChunkCount() const;

    /// WorldGenUtil.isPosInWorldGenRange.
    static bool isPosInWorldGenRange(
        sec::Pos     requestedPos,
        std::int32_t centerChunkX,
        std::int32_t centerChunkZ,
        std::int32_t maxChunkRadius
    );

    //==============//
    // base methods //
    //==============//

    /// Disposes every section and detaches from the provider. Retrieval futures only capture the shared
    /// RetrievalState (safe after destruction); the per-tick queuing task on `retrievalQueueExecutor` captures
    /// the tree behind an alive flag, so drain/stop that executor BEFORE destroying the tree.
    void close();

private:
    struct ReloadQueue {
        std::mutex           mutex;
        std::deque<sec::Pos> positions;
    };

    /// Java missingGenerationPosSet / queuedGenerationPosSet (+ the HUD values). Shared with the retrieval
    /// futures' callbacks so a completion after the tree is gone only touches this object.
    struct RetrievalState {
        mutable std::mutex        mutex;
        std::set<sec::Pos>        missing; ///< ordered: deterministic near-to-far sort ties
        std::set<sec::Pos>        queued;
        std::atomic<bool>         canRegenerate{true};
        std::atomic<std::int64_t> estimatedRemainingChunks{0};
        PositionsToRetrieveFn     positionsToRetrieve;

        /// LodQuadTree.tryQueuePosForRetrieval :1020-1037
        void tryQueuePosForRetrieval(sec::Pos pos);
        /// genFuture.exceptionally / thenAccept :905-926
        void onRetrievalComplete(sec::Pos missingPos, const RetrievalResult& result);
    };

    void updateDetailLevelVariables();
    void updateAllRenderSections(BlockPos2D playerPos);

    bool recursivelyUpdateRenderSectionNode(
        BlockPos2D playerPos,
        Node*      rootNode,
        Node*      parentNode,
        Node*      quadNode,
        sec::Pos   sectionPos
    );
    bool  onDetailLevelTooLow(BlockPos2D playerPos, Node* rootNode, Node* quadNode);
    bool  onDesiredDetailLevel(Node* quadNode, Node* parentNode);
    Node* tryAddNodeToTree(Node* rootNode, Node* quadNode, sec::Pos sectionPos);
    void  recursivelyDisableChildNodes(Node* quadNode);

    void reloadQueuedSections();
    void loadQueuedSections(BlockPos2D playerPos, const std::vector<Section>& nodesNeedingLoading);

    bool threadPoolCanAcceptWorldGenTasks() const;
    bool canQueueRetrievalNowImpl() const;
    void queueWorldGen(BlockPos2D playerPos);
    void runRetrievalQueuing(BlockPos2D playerPos, const std::vector<Section>& worldGenSections);
    void startQueuedRetrievalTasks(BlockPos2D playerPos);

    Section makeSection(sec::Pos pos);

    const Options*        options_;
    RenderSectionContext& ctx_;
    LodQuadTreeDeps       deps_;

    std::mutex        treeTickMutex_;
    std::atomic<bool> tickAborted_{false};

    /// detail math / zoom / last player pos, shared with the sections
    std::shared_ptr<TreeShared> shared_ = std::make_shared<TreeShared>();
    /// snapshots taken at the start of a tick, read by the walk (tick thread only)
    DetailLevelMath tickMath_;
    CameraZoom      tickZoom_;
    /// getCenterBlockPos() published for other threads (TreeShared::pack)
    std::atomic<std::int64_t> centerBlockPosAtomic_{0};

    std::shared_ptr<ReloadQueue> sectionsToReload_ = std::make_shared<ReloadQueue>();
    SourceProvider*              attachedProvider_ = nullptr;
    std::optional<ListenerId>    providerListener_;

    std::atomic<bool>                  requeueAllRetrievalTasks_{false};
    std::atomic<bool>                  queueThreadRunning_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    std::shared_ptr<RetrievalState> retrieval_ = std::make_shared<RetrievalState>();

    TickNodeHolder tickNodeHolder_;

    mutable std::mutex   enabledMutex_;
    std::vector<Section> enabledSections_;
    std::vector<Section> altEnabledSections_;
};

} // namespace dh::tree
