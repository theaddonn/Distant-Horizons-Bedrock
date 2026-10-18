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

//============//
// CameraZoom //
//============//

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

//==================//
// detail level math //
//==================//

/// LodQuadTree.updateDetailLevelVariables + calcDetailLevelFromDistance + calcExpectedDetailLevel as a value
/// type (also what the `treemath` golden suite replays). All detail levels are ABSOLUTE (0 = block); add
/// SECTION_MINIMUM_DETAIL_LEVEL for section detail levels.
struct DetailLevelMath {
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
    /// FullDataUpdatePropagatorV2.HIGHEST_DOWNSAMPLE_DETAIL_LEVEL: sections at/above never zoom.
    static constexpr std::int8_t kHighestDownsampleDetailLevel = sec::SECTION_MINIMUM_DETAIL_LEVEL + 6;
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
    BlockPos2D lastTickPlayerPos() const;

    DetailLevelMath detailLevelMath() const;
    CameraZoom      cameraZoom() const;
    /// Zoom magnification hook (Java CameraZoom.INSTANCE.update(renderState) every frame).
    void setCameraZoom(const CameraZoom& zoom);

    const RenderSectionContext& context() const { return ctx_; }

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
    std::int8_t calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos) const;
    std::int8_t
    calcExpectedDetailLevel(BlockPos2D playerPos, std::int32_t targetBlockPosX, std::int32_t targetBlockPosZ) const;

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
    /// Java fullDataSourceProvider.setCanRegenerate(...) value: false while block-detail LODs are missing.
    bool canRegenerate() const { return canRegenerate_.load(); }
    /// Java setEstimatedRemainingRetrievalChunkCount (missing positions only, see header).
    std::int64_t estimatedRemainingRetrievalChunkCount() const { return estimatedRemainingChunks_.load(); }

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

    /// Disposes every section and detaches from the provider. Shut down the retrieval queue / executors that
    /// may still call back into the tree BEFORE calling this (their futures capture the tree).
    void close();

private:
    struct ReloadQueue {
        std::mutex           mutex;
        std::deque<sec::Pos> positions;
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
    void tryQueuePosForRetrieval(sec::Pos pos);
    void onRetrievalComplete(sec::Pos missingPos, const RetrievalResult& result);

    Section makeSection(sec::Pos pos);

    const Options*        options_;
    RenderSectionContext& ctx_;
    LodQuadTreeDeps       deps_;

    std::mutex        treeTickMutex_;
    std::atomic<bool> tickAborted_{false};

    mutable std::mutex detailMutex_;
    DetailLevelMath    detailMath_;
    CameraZoom         zoom_;
    /// snapshots taken at the start of a tick, read by the walk (tick thread only)
    DetailLevelMath           tickMath_;
    CameraZoom                tickZoom_;
    std::atomic<std::int64_t> lastPlayerPos_{0}; ///< packed x (high 32) / z (low 32)

    std::shared_ptr<ReloadQueue> sectionsToReload_ = std::make_shared<ReloadQueue>();
    SourceProvider*              attachedProvider_ = nullptr;
    std::optional<ListenerId>    providerListener_;

    std::atomic<bool>                  requeueAllRetrievalTasks_{false};
    std::atomic<bool>                  queueThreadRunning_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    mutable std::mutex        retrievalMutex_;
    std::set<sec::Pos>        missingGenerationPosSet_; ///< ordered: deterministic near-to-far sort ties
    std::set<sec::Pos>        queuedGenerationPosSet_;
    std::atomic<bool>         canRegenerate_{true};
    std::atomic<std::int64_t> estimatedRemainingChunks_{0};

    TickNodeHolder tickNodeHolder_;

    mutable std::mutex   enabledMutex_;
    std::vector<Section> enabledSections_;
    std::vector<Section> altEnabledSections_;
};

} // namespace dh::tree
