#include "dh/tree/LodQuadTree.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "dh/fulldata/Merge.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/ChunkPos.h"
#include "dh/util/Bits.h"
#include "dh/util/MathUtil.h"

namespace dh::tree {

//============//
// CameraZoom //
//============//

bool CameraZoom::coneIntersectsCircle(
    double coneOriginX,
    double coneOriginZ,
    double circleCenterX,
    double circleCenterZ,
    double circleRadius
) const {
    const double offsetX = circleCenterX - coneOriginX;
    const double offsetZ = circleCenterZ - coneOriginZ;

    const double distanceAlongLook = (offsetX * lookDirectionX) + (offsetZ * lookDirectionZ);
    if (distanceAlongLook < -circleRadius) {
        // entirely behind the camera
        return false;
    }

    // 2D cross product: how far the circle's center is from the camera's look line
    const double distanceAcrossLook = std::fabs((offsetX * lookDirectionZ) - (offsetZ * lookDirectionX));
    return distanceAcrossLook <= (distanceAlongLook * coneTanHalfAngle) + circleRadius;
}

CameraZoom CameraZoom::fromProjection(
    const glm::mat4&  projection,
    const glm::vec3&  lookAt,
    double            fovSettingDegrees,
    HorizontalQuality horizontalQuality,
    int               maxZoomQualityIncrease,
    bool              increaseQualityWhenZoomedIn
) {
    if (!increaseQualityWhenZoomedIn) {
        return notZoomed();
    }

    // DhApiMat4f.m1c (row 1) == glm projection[c][1]; MathUtil.pow2(float) sums in float, sqrt in double
    const float  row1 = math::pow2(projection[0][1]) + math::pow2(projection[1][1]) + math::pow2(projection[2][1]);
    const double projectionYScale = std::sqrt(static_cast<double>(row1));
    const double pi               = 3.141592653589793;
    const double fovSettingYScale = 1.0 / std::tan((fovSettingDegrees / 180.0 * pi) / 2.0);

    // how many times larger objects appear on screen compared to the player's FOV setting
    double magnification = projectionYScale / fovSettingYScale;
    if (magnification < MIN_ZOOM_MAGNIFICATION) {
        // ignores minor FOV reductions, FOV increases and non-perspective projections
        return notZoomed();
    }

    // limit how much additional detail a strong zoom can request
    const double maxMagnification =
        std::pow(info(horizontalQuality).quadraticBase, static_cast<double>(maxZoomQualityIncrease));
    magnification = jmind(magnification, maxMagnification);

    // LOD detail is selected in 2D so only the look direction's horizontal component matters
    const float  lookLenSq    = math::pow2(lookAt.x) + math::pow2(lookAt.z);
    const double lookLengthXZ = std::sqrt(static_cast<double>(lookLenSq));
    if (lookLengthXZ < 0.1) {
        // looking almost straight up or down
        return notZoomed();
    }

    const float  row0 = math::pow2(projection[0][0]) + math::pow2(projection[1][0]) + math::pow2(projection[2][0]);
    const double projectionXScale = std::sqrt(static_cast<double>(row0));
    const double coneTanHalfAngle = (1.0 / projectionXScale) * ZOOM_CONE_PADDING_MULTIPLIER;

    CameraZoom zoom;
    zoom.magnification    = magnification;
    zoom.coneTanHalfAngle = coneTanHalfAngle;
    zoom.lookDirectionX   = static_cast<double>(lookAt.x) / lookLengthXZ;
    zoom.lookDirectionZ   = static_cast<double>(lookAt.z) / lookLengthXZ;
    return zoom;
}

//=================//
// DetailLevelMath //
//=================//

DetailLevelMath DetailLevelMath::compute(
    HorizontalQuality       horizontalQuality,
    MaxHorizontalResolution maxHorizontalResolution,
    std::int32_t            blockRenderDistanceDiameter,
    std::int8_t             treeRootDetailLevel
) {
    // Note: these detail levels are all absolute, not section (LodQuadTree.java:1246-1262).
    DetailLevelMath m;
    m.treeRootDetailLevel = treeRootDetailLevel;
    m.detailDropOffDistanceUnit =
        static_cast<double>(wmul(info(horizontalQuality).distanceUnitInBlocks, lod::CHUNK_WIDTH));
    m.detailDropOffLogBase     = std::log(info(horizontalQuality).quadraticBase);
    m.maxLeafRenderDetailLevel = static_cast<std::int8_t>(detailLevel(maxHorizontalResolution));

    // The minimum detail level prevents single corner sections rendering 1 detail level lower than the others.
    std::int8_t minSectionDetailLevel = m.calcDetailLevelFromDistance(static_cast<double>(blockRenderDistanceDiameter));
    minSectionDetailLevel =
        static_cast<std::int8_t>(minSectionDetailLevel - 1); // corners can't render lower than their neighbours
    minSectionDetailLevel =
        math::min(minSectionDetailLevel, treeRootDetailLevel); // no lower detail than the tree contains
    minSectionDetailLevel = math::min(
        minSectionDetailLevel,
        static_cast<std::int8_t>(ROOT_SECTION_DETAIL_LEVEL - sec::SECTION_BLOCK_DETAIL_LEVEL)
    ); // the tree can only provide LODs as low as the provider has
    // respect the user's selected max resolution if it is lower detail
    m.minRootRenderDetailLevel = math::max(minSectionDetailLevel, m.maxLeafRenderDetailLevel);
    return m;
}

std::int8_t DetailLevelMath::calcDetailLevelFromDistance(double blockDistance, std::int8_t maxDetailLevel) const {
    // LodQuadTree.java:1269: (int) truncates toward zero; NaN -> 0, -inf -> INT_MIN, then clamp
    const std::int32_t detailLevel =
        satcast_i32(std::log(blockDistance / detailDropOffDistanceUnit) / detailDropOffLogBase);
    return static_cast<std::int8_t>(
        math::clamp(static_cast<std::int32_t>(maxDetailLevel), detailLevel, ROOT_SECTION_DETAIL_LEVEL)
    );
}

std::int8_t
DetailLevelMath::calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos, const CameraZoom& zoom) const {
    // the radius (half diagonal) so the zoom cone check doesn't miss partially overlapping sections
    const double sectionBlockRadius = sec::blockWidth(sectionPos) * (std::sqrt(2.0) / 2.0);
    return calcExpectedDetailLevel(
        playerPos,
        sectionPos,
        sec::centerBlockX(sectionPos),
        sec::centerBlockZ(sectionPos),
        sectionBlockRadius,
        zoom
    );
}

std::int8_t DetailLevelMath::calcExpectedDetailLevel(
    BlockPos2D        playerPos,
    std::int32_t      targetBlockPosX,
    std::int32_t      targetBlockPosZ,
    const CameraZoom& zoom
) const {
    return calcExpectedDetailLevel(
        playerPos,
        sec::encodeContaining(sec::SECTION_BLOCK_DETAIL_LEVEL, BlockPos(targetBlockPosX, 0, targetBlockPosZ)),
        targetBlockPosX,
        targetBlockPosZ,
        0.0,
        zoom
    );
}

std::int8_t DetailLevelMath::calcExpectedDetailLevel(
    BlockPos2D        playerPos,
    sec::Pos          targetSectionPos,
    std::int32_t      targetBlockPosX,
    std::int32_t      targetBlockPosZ,
    double            targetBlockRadius,
    const CameraZoom& zoom
) const {
    double blockDistance = playerPos.dist(targetBlockPosX, targetBlockPosZ);

    // Don't allow zooming for LODs that will be missing child nodes (holes / missing LODs).
    const bool allowZooming = sec::detail(targetSectionPos) < LodQuadTree::kHighestDownsampleDetailLevel;
    if (allowZooming) {
        // LODs visible through a zoomed camera appear closer: use the magnified distance
        if (zoom.isZoomed()
            && zoom.coneIntersectsCircle(
                playerPos.x,
                playerPos.z,
                targetBlockPosX,
                targetBlockPosZ,
                targetBlockRadius
            )) {
            blockDistance /= zoom.magnification;
            // Java: calcDetailLevelFromDistance(blockDistance, maxHorizontalResolution.detailLevel) - same value
            return calcDetailLevelFromDistance(blockDistance, maxLeafRenderDetailLevel);
        }
    }
    return calcDetailLevelFromDistance(blockDistance);
}

//=============//
// LodQuadTree //
//=============//

namespace {
std::int64_t packPos(BlockPos2D p) {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) << 32) | static_cast<std::uint32_t>(p.z)
    );
}
BlockPos2D unpackPos(std::int64_t v) {
    return BlockPos2D(
        static_cast<std::int32_t>(static_cast<std::uint64_t>(v) >> 32),
        static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)))
    );
}
} // namespace

std::int32_t LodQuadTree::viewDiameterInBlocks(const Options& options) {
    return wmul(wmul(options.client.graphics.quality.lodChunkRenderDistanceRadius, lod::CHUNK_WIDTH), 2);
}

std::int8_t LodQuadTree::rootDetailLevelFor(std::int32_t viewDiameter) {
    const std::int32_t a = std::max<std::int32_t>(1, sec::SECTION_MINIMUM_DETAIL_LEVEL);
    const std::int32_t b = math::log2(viewDiameter);
    return static_cast<std::int8_t>(std::max(a, b));
}

LodQuadTree::LodQuadTree(
    const Options&        options,
    std::int32_t          viewDiameter,
    BlockPos2D            initialPlayerBlockPos,
    RenderSectionContext& ctx,
    LodQuadTreeDeps       deps
)
: Base(viewDiameter, kBlockDistanceForNodeClearing, initialPlayerBlockPos, sec::SECTION_MINIMUM_DETAIL_LEVEL),
  blockRenderDistanceDiameter(viewDiameter),
  options_(&options),
  ctx_(ctx),
  deps_(std::move(deps)) {
    lastPlayerPos_.store(packPos(initialPlayerBlockPos));
    updateDetailLevelVariables();
}

LodQuadTree::~LodQuadTree() { close(); }

//==================//
// property getters //
//==================//

void LodQuadTree::collectEnabled(std::vector<Section>& out) const {
    std::lock_guard lock(enabledMutex_);
    out.clear();
    out.insert(out.end(), enabledSections_.begin(), enabledSections_.end());
}

BlockPos2D LodQuadTree::lastTickPlayerPos() const { return unpackPos(lastPlayerPos_.load()); }

DetailLevelMath LodQuadTree::detailLevelMath() const {
    std::lock_guard lock(detailMutex_);
    return detailMath_;
}

CameraZoom LodQuadTree::cameraZoom() const {
    std::lock_guard lock(detailMutex_);
    return zoom_;
}

void LodQuadTree::setCameraZoom(const CameraZoom& zoom) {
    std::lock_guard lock(detailMutex_);
    zoom_ = zoom;
}

std::int8_t LodQuadTree::calcExpectedDetailLevel(BlockPos2D playerPos, sec::Pos sectionPos) const {
    DetailLevelMath m;
    CameraZoom      z;
    {
        std::lock_guard lock(detailMutex_);
        m = detailMath_;
        z = zoom_;
    }
    return m.calcExpectedDetailLevel(playerPos, sectionPos, z);
}

std::int8_t LodQuadTree::calcExpectedDetailLevel(
    BlockPos2D   playerPos,
    std::int32_t targetBlockPosX,
    std::int32_t targetBlockPosZ
) const {
    DetailLevelMath m;
    CameraZoom      z;
    {
        std::lock_guard lock(detailMutex_);
        m = detailMath_;
        z = zoom_;
    }
    return m.calcExpectedDetailLevel(playerPos, targetBlockPosX, targetBlockPosZ, z);
}

void LodQuadTree::updateDetailLevelVariables() {
    const QualityOptions& q = options_->client.graphics.quality;
    std::lock_guard       lock(detailMutex_);
    detailMath_ = DetailLevelMath::compute(
        q.horizontalQuality,
        q.maxHorizontalResolution,
        blockRenderDistanceDiameter,
        treeRootDetailLevel
    );
}

LodQuadTree::Section LodQuadTree::makeSection(sec::Pos pos) {
    return std::make_shared<LodRenderSection>(pos, *this, ctx_);
}

//=============//
// tick update //
//=============//

void LodQuadTree::tryTick(BlockPos2D playerPos) {
    // don't tick the tree if a modification is still going
    std::unique_lock lock(treeTickMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (!alive_->load()) {
        return;
    }

    // this shouldn't be updated while the tree is being iterated through
    updateDetailLevelVariables();
    lastPlayerPos_.store(packPos(playerPos));
    {
        // tick-thread snapshots (Java read the live Config / CameraZoom.INSTANCE during the walk)
        std::lock_guard dlock(detailMutex_);
        tickMath_ = detailMath_;
        tickZoom_ = zoom_;
    }

    tickAborted_.store(false);
    updateAllRenderSections(playerPos);
}

void LodQuadTree::updateAllRenderSections(BlockPos2D playerPos) {
    // this data will be updated as we walk through the tree
    tickNodeHolder_.clear();

    //===================//
    // recenter the tree //
    //===================//

    setCenterBlockPos(
        playerPos,
        // remove completely out of bound nodes (the root node is no longer in bounds)
        [&](Value& value) {
            if (value.has_value() && *value) {
                const Section& renderSection = *value;
                const sec::Pos removedPos    = renderSection->pos;
                if (deps_.retrievalQueue != nullptr) {
                    deps_.retrievalQueue->removeRetrievalRequestIf([removedPos](std::uint64_t genPos) {
                        return sec::contains(removedPos, genPos);
                    });
                }
                {
                    // a removed position may be larger than the generated positions it contains
                    std::lock_guard rlock(retrievalMutex_);
                    std::erase_if(missingGenerationPosSet_, [&](sec::Pos genPos) {
                        return sec::contains(removedPos, genPos);
                    });
                    std::erase_if(queuedGenerationPosSet_, [&](sec::Pos genPos) {
                        return sec::contains(removedPos, genPos);
                    });
                }
                renderSection->dispose();
            }
        },
        // mutate partially out of bound nodes (the root node is still in bounds, but this child node isn't)
        [&](Value& value) {
            if (value.has_value() && *value) {
                // when this node comes back into render distance we'll need to re-load it since the full
                // data may have been modified while it was out of bounds
                (*value)->renderDataDirty.store(true);
            }
        }
    );

    //=======================//
    // walk through the tree //
    //=======================//

    RootPosIterator rootPosIterator = rootNodePosIterator();
    while (rootPosIterator.hasNext()) {
        // make sure all root nodes have been created
        const sec::Pos rootPos = rootPosIterator.nextLong();
        if (getNode(rootPos) == nullptr) {
            setValue(rootPos, makeSection(rootPos));
        }

        Node* rootNode = getNode(rootPos);
        if (rootNode == nullptr) {
            log::error("LodQuadTree: all root nodes should have been created by this point.");
            tickAborted_.store(true);
            break;
        }
        recursivelyUpdateRenderSectionNode(playerPos, rootNode, nullptr, rootNode, rootNode->sectionPos);
        if (tickAborted_.load()) {
            break;
        }
    }
    if (tickAborted_.load()) {
        // Java: the IllegalStateException unwound updateAllRenderSections; tryTick logged it and nothing
        // below ran this tick
        return;
    }

    //============//
    // queue work //
    //============//

    if (requeueAllRetrievalTasks_.exchange(false)) {
        NodeIterator it = nodeIterator();
        while (it.hasNext()) {
            Node* node = it.next();
            if (node == nullptr || !node->value.has_value() || !*node->value) {
                continue;
            }
            (*node->value)->queuedMissingSectionsForRetrieval.store(false);
        }
    }

    // reloading is for sections that have been loaded once already
    reloadQueuedSections();

    // loading is for sections that haven't rendered yet
    loadQueuedSections(playerPos, tickNodeHolder_.getLoadSections());

    //==================//
    // enable rendering //
    //==================//

    altEnabledSections_.clear();

    for (Node* node : tickNodeHolder_.getEnabledNodes()) {
        if (node == nullptr || !node->value.has_value() || !*node->value) continue;
        (*node->value)->setRenderingEnabled(true);
        altEnabledSections_.push_back(*node->value);
    }
    for (Node* node : tickNodeHolder_.getEnableDeleteChildrenNodes()) {
        if (node == nullptr || !node->value.has_value() || !*node->value) continue;
        (*node->value)->setRenderingEnabled(true);
        altEnabledSections_.push_back(*node->value);
    }

    //====================//
    // update render list //
    //====================//

    {
        std::lock_guard lock(enabledMutex_);
        std::swap(enabledSections_, altEnabledSections_);
    }

    //=========================//
    // node disabling/deletion //
    //=========================//

    for (Node* node : tickNodeHolder_.getDisableNodes()) {
        if (node == nullptr || !node->value.has_value() || !*node->value) continue;
        (*node->value)->setRenderingEnabled(false);
    }

    for (Node* node : tickNodeHolder_.getEnableDeleteChildrenNodes()) {
        if (node == nullptr || !node->value.has_value()
            || !*node->value
            // only clear the children if there are children to clear
            || node->getDirectChildCount() == 0) {
            continue;
        }

        // Running here (not on the render thread) prevents infinitely growing lod-loading tasks
        // (LodQuadTree.java:440-447).
        node->deleteAllChildren([](Value& childValue) {
            if (childValue.has_value() && *childValue) {
                (*childValue)->setRenderingEnabled(false);
                (*childValue)->dispose();
            }
        });
    }

    //=================//
    // beacon enabling //
    //=================//

    // tryRefreshRenderingBeaconsAsync: not ported (no beacon renderer in this port yet)

    //===================//
    // world gen queuing //
    //===================//

    queueWorldGen(playerPos);
}

//=========================//
// tick - recursive update //
//=========================//

bool LodQuadTree::recursivelyUpdateRenderSectionNode(
    BlockPos2D playerPos,
    Node*      rootNode,
    Node*      parentNode,
    Node*      quadNode,
    sec::Pos   sectionPos
) {
    //=====================//
    // get/create the node //
    //=====================//

    quadNode = tryAddNodeToTree(rootNode, quadNode, sectionPos);
    if (quadNode == nullptr) {
        return false; // tick aborted
    }

    // Skip sections that are out-of-bounds, otherwise some sections appear/generate outside the render distance.
    if (!isSectionPosInBounds(quadNode->sectionPos)) {
        tickNodeHolder_.addDisableNode(quadNode);
        recursivelyDisableChildNodes(quadNode);
        return true;
    }

    // make sure the render section is created (shouldn't be necessary, but just in case)
    if (!quadNode->value.has_value() || !*quadNode->value) {
        quadNode->setValue(sectionPos, makeSection(sectionPos));
    }
    const Section& renderSection = *quadNode->value;

    //===============================//
    // handle enabling, loading,     //
    // and disabling render sections //
    //===============================//

    // load every node for rendering
    if (!renderSection->gpuUploadInProgress() && !renderSection->gpuUploadComplete()) {
        tickNodeHolder_.addLoadSection(renderSection);
    }

    std::int8_t expectedDetailLevel = tickMath_.calcExpectedDetailLevel(playerPos, quadNode->sectionPos, tickZoom_);
    expectedDetailLevel             = math::min(expectedDetailLevel, tickMath_.minRootRenderDetailLevel);
    expectedDetailLevel             = static_cast<std::int8_t>(expectedDetailLevel + sec::SECTION_MINIMUM_DETAIL_LEVEL);

    const std::int8_t nodeDetail = sec::detail(quadNode->sectionPos);
    if (nodeDetail > expectedDetailLevel) {
        return onDetailLevelTooLow(playerPos, rootNode, quadNode);
    }
    // the (expectedDetailLevel-1) fixes corners being cut out due to distance calculations using the LOD center
    else if (nodeDetail == expectedDetailLevel || nodeDetail == expectedDetailLevel - 1) {
        return onDesiredDetailLevel(quadNode, parentNode);
    } else {
        // A zoomed in camera boosts detail directionally: out-of-cone children of a node that intersects the
        // zoom cone can end up several detail levels finer than their own distance alone would request.
        if (tickZoom_.isZoomed()) {
            return onDesiredDetailLevel(quadNode, parentNode);
        }

        // Java: IllegalStateException, caught by tryTick (the rest of this tick is skipped)
        log::logf(
            LogLevel::Error,
            "LodQuadTree shouldn't be updating renderSections below the expected detail level: [%d] at [%s].",
            static_cast<int>(expectedDetailLevel),
            sec::toString(quadNode->sectionPos).c_str()
        );
        tickAborted_.store(true);
        return false;
    }
}

bool LodQuadTree::onDetailLevelTooLow(BlockPos2D playerPos, Node* rootNode, Node* quadNode) {
    // recursively update each child node
    int childNodeRenderCount = 0;
    for (int i = 0; i < 4; i++) {
        const sec::Pos childPos  = sec::child(quadNode->sectionPos, i);
        Node*          childNode = quadNode->getChildByIndex(i);

        const bool childCanRender =
            recursivelyUpdateRenderSectionNode(playerPos, rootNode, quadNode, childNode, childPos);
        if (tickAborted_.load()) {
            return false;
        }
        if (childCanRender) {
            childNodeRenderCount++;
        }
    }

    const bool isRootNode = (quadNode == rootNode);
    if (isRootNode) {
        // Never render the root node (LodQuadTree.java:676-682): prevents flashing when moving across root
        // node boundaries, where new empty nodes at the tree edge would otherwise need the root to cover them.
        tickNodeHolder_.addDisableNode(quadNode);
        return false;
    } else if (childNodeRenderCount >= 4) {
        // all children can render, the area will be filled when rendering (:684-690)
        tickNodeHolder_.addDisableNode(quadNode);
        return true;
    } else {
        const bool nodeCanRender = quadNode->value.has_value() && *quadNode->value && (*quadNode->value)->canRender();
        if (nodeCanRender) {
            // not all child positions are loaded yet, this one should be rendered instead (:694-700)
            tickNodeHolder_.addEnableNode(quadNode);
            recursivelyDisableChildNodes(quadNode);
        } else {
            tickNodeHolder_.addDisableNode(quadNode);
        }
        return nodeCanRender;
    }
}

bool LodQuadTree::onDesiredDetailLevel(Node* quadNode, Node* parentNode) {
    // Skip sections that are out-of-bounds (:716-722)
    if (!isSectionPosInBounds(quadNode->sectionPos)) {
        return true;
    }

    if (quadNode->value.has_value() && *quadNode->value && (*quadNode->value)->canRender()) {
        if (!tickNodeHolder_.isEnableNode(parentNode)) {
            tickNodeHolder_.addEnableDeleteChildrenNode(quadNode); // :727-730
            return true;
        } else {
            tickNodeHolder_.addDisableNode(quadNode);
            return false;
        }
    } else {
        tickNodeHolder_.addDisableNode(quadNode);
        return false;
    }
}

LodQuadTree::Node* LodQuadTree::tryAddNodeToTree(Node* rootNode, Node* quadNode, sec::Pos sectionPos) {
    // create the node
    if (quadNode == nullptr) {
        rootNode->setValue(sectionPos, makeSection(sectionPos));
        quadNode = rootNode->getNode(sectionPos);
    }
    if (quadNode == nullptr) {
        // Java: LodUtil.assertNotReach (throws -> tick aborted)
        log::logf(
            LogLevel::Error,
            "Unable to add node with pos [%s] to tree root [%s].",
            sec::toString(sectionPos).c_str(),
            sec::toString(rootNode->sectionPos).c_str()
        );
        tickAborted_.store(true);
    }
    return quadNode;
}

void LodQuadTree::recursivelyDisableChildNodes(Node* quadNode) {
    for (int i = 0; i < 4; i++) {
        Node* childNode = quadNode->getChildByIndex(i);
        tickNodeHolder_.removeEnableAndDisableNode(childNode);

        if (childNode != nullptr) {
            recursivelyDisableChildNodes(childNode);
        }
    }
}

//=====================//
// tick - work queuing //
//=====================//

void LodQuadTree::reloadQueuedSections() {
    std::deque<sec::Pos> drained;
    {
        std::lock_guard lock(sectionsToReload_->mutex);
        drained.swap(sectionsToReload_->positions);
    }

    std::set<sec::Pos>    positionsToRequeue;
    std::vector<sec::Pos> requeueOrder;
    for (const sec::Pos pos : drained) {
        if (positionsToRequeue.contains(pos)) {
            // don't attempt to re-load positions that are already in the process of reloading
            continue;
        }

        const Value value = tryGetValue(pos);
        if (value.has_value() && *value) {
            const Section& renderSection = *value;
            // the section only needs to be updated if a buffer is currently present
            if (renderSection->gpuUploadComplete()) {
                if (renderSection->gpuUploadInProgress() || !renderSection->uploadRenderDataToGpuAsync()) {
                    // already loading or failed to start: trigger it again later or the LOD stays out of date
                    if (positionsToRequeue.insert(pos).second) {
                        requeueOrder.push_back(pos);
                    }
                }
            }
        }
    }

    if (!requeueOrder.empty()) {
        std::lock_guard lock(sectionsToReload_->mutex);
        sectionsToReload_->positions
            .insert(sectionsToReload_->positions.end(), requeueOrder.begin(), requeueOrder.end());
    }
}

void LodQuadTree::loadQueuedSections(BlockPos2D playerPos, const std::vector<Section>& nodesNeedingLoading) {
    std::vector<Section> loadSectionList = nodesNeedingLoading;
    std::stable_sort(loadSectionList.begin(), loadSectionList.end(), [&](const Section& a, const Section& b) {
        // lower-detail LODs first (larger numbers first)
        const std::int8_t aDetailLevel = sec::detail(a->pos);
        const std::int8_t bDetailLevel = sec::detail(b->pos);
        if (aDetailLevel != bDetailLevel) {
            return bDetailLevel < aDetailLevel;
        }
        // closer LODs first
        return sec::manhattanBlockDistance(a->pos, playerPos) < sec::manhattanBlockDistance(b->pos, playerPos);
    });

    for (const Section& renderSection : loadSectionList) {
        if (!renderSection->gpuUploadInProgress() && !renderSection->gpuUploadComplete()) {
            renderSection->uploadRenderDataToGpuAsync();
        }
    }
}

//=================================//
// full data retrieval (world gen) //
//=================================//

bool LodQuadTree::threadPoolCanAcceptWorldGenTasks() const {
    const std::size_t maxPropagateTaskCount = static_cast<std::size_t>(
        std::max(0, kParentUpdateTasksPerThread * options_->common.multiThreading.numberOfThreads)
    );

    // don't queue additional world gen requests if the render loader is busy
    if (ctx_.renderLoader == nullptr || ctx_.renderLoader->queueSize() >= maxPropagateTaskCount / 2) {
        return false;
    }
    // don't queue additional world gen requests if the file handler is busy
    if (deps_.fileHandler != nullptr && deps_.fileHandler->queueSize() >= maxPropagateTaskCount / 2) {
        return false;
    }
    return true;
}

bool LodQuadTree::canQueueRetrievalNowImpl() const {
    if (deps_.retrievalQueue == nullptr) return false;
    return deps_.canQueueRetrievalNow ? deps_.canQueueRetrievalNow() : true;
}

void LodQuadTree::queueWorldGen(BlockPos2D playerPos) {
    if (!threadPoolCanAcceptWorldGenTasks() || !canQueueRetrievalNowImpl() || queueThreadRunning_.load()) {
        return;
    }
    queueThreadRunning_.store(true); // don't run multiple threads at once

    // needs to be collected outside the queue thread to prevent concurrency issues
    std::vector<Section> worldGenSections = tickNodeHolder_.getWorldGenSectionsNearToFar(playerPos);

    std::shared_ptr<std::atomic<bool>> alive = alive_;
    auto task = [this, alive, playerPos, worldGenSections = std::move(worldGenSections)]() {
        if (!alive->load()) return;
        runRetrievalQueuing(playerPos, worldGenSections);
    };

    if (deps_.retrievalQueueExecutor != nullptr) {
        // running on a separate thread allows for faster loading of finished LODs
        if (!deps_.retrievalQueueExecutor->execute(std::move(task))) {
            queueThreadRunning_.store(false);
        }
    } else {
        task();
    }
}

void LodQuadTree::runRetrievalQueuing(BlockPos2D playerPos, const std::vector<Section>& worldGenSections) {
    for (const Section& section : worldGenSections) {
        // since this section wants to render, check if it needs any generation to do so
        if (!section->queuedMissingSectionsForRetrieval.exchange(true)) {
            tryQueuePosForRetrieval(section->pos); // can be quite slow
        }
    }

    // Java looped "startQueuedRetrievalTasks; sleep 100 ms while work remains"; one pass per 100 ms tick here.
    startQueuedRetrievalTasks(playerPos);

    queueThreadRunning_.store(false);
}

void LodQuadTree::startQueuedRetrievalTasks(BlockPos2D playerPos) {
    // sort the nodes from nearest to farthest (ties keep the set's ascending pos order)
    std::vector<sec::Pos> sortedMissingPosList;
    {
        std::lock_guard lock(retrievalMutex_);
        sortedMissingPosList.assign(missingGenerationPosSet_.begin(), missingGenerationPosSet_.end());
    }
    std::stable_sort(sortedMissingPosList.begin(), sortedMissingPosList.end(), [&](sec::Pos posA, sec::Pos posB) {
        return sec::manhattanBlockDistance(posA, playerPos) < sec::manhattanBlockDistance(posB, playerPos);
    });

    //==================================//
    // add retrieval tasks to the queue //
    //==================================//

    const WorldGeneratorOptions& wg = options_->common.worldGenerator;
    for (const sec::Pos missingPos : sortedMissingPosList) {
        if (!canQueueRetrievalNowImpl()) {
            break;
        }

        // is this position within acceptable generator range?
        if (!isPosInWorldGenRange(
                missingPos,
                wg.generationCenterChunkX,
                wg.generationCenterChunkZ,
                wg.generationMaxChunkRadius
            )) {
            continue;
        }

        // GeneratedFullDataSourceProvider.queuePositionForRetrieval: required data detail = section detail - 6
        std::shared_ptr<RetrievalFuture> genFuture = deps_.retrievalQueue->submit(
            missingPos,
            static_cast<std::uint8_t>(sec::detail(missingPos) - sec::SECTION_MINIMUM_DETAIL_LEVEL)
        );
        bool positionQueued = genFuture != nullptr;
        if (positionQueued && genFuture->isDone()) {
            const std::optional<RetrievalResult> r = genFuture->get();
            // Java: !genFuture.isCompletedExceptionally()
            positionQueued =
                r.has_value() && (r->kind == RetrievalResult::Kind::Success || r->kind == RetrievalResult::Kind::Split);
        }
        if (positionQueued) {
            {
                std::lock_guard lock(retrievalMutex_);
                queuedGenerationPosSet_.insert(missingPos);
                missingGenerationPosSet_.erase(missingPos);
            }
            std::shared_ptr<std::atomic<bool>> alive = alive_;
            genFuture->onComplete([this, alive, missingPos](const RetrievalResult& result) {
                if (!alive->load()) return;
                onRetrievalComplete(missingPos, result);
            });
        }
    }

    //==========================//
    // calc task count estimate //
    //==========================//

    std::int64_t totalWorldGenChunkCount = 0;
    if (!sortedMissingPosList.empty()) {
        // don't let any regeneration happen until all low-detail LODs have finished generating
        bool highDetailRequestPresent = true;
        for (const sec::Pos missingPos : sortedMissingPosList) {
            const std::int64_t sectionWidthInChunks  = sec::chunkWidth(missingPos);
            totalWorldGenChunkCount                 += sectionWidthInChunks * sectionWidthInChunks;
            if (sec::detail(missingPos) == sec::SECTION_BLOCK_DETAIL_LEVEL) {
                highDetailRequestPresent = false;
            }
        }
        canRegenerate_.store(!highDetailRequestPresent);
    } else {
        // Java also estimated the regen chunk count from the DB here (not ported)
        canRegenerate_.store(true);
    }
    estimatedRemainingChunks_.store(totalWorldGenChunkCount);
}

void LodQuadTree::onRetrievalComplete(sec::Pos missingPos, const RetrievalResult& result) {
    {
        std::lock_guard lock(retrievalMutex_);
        queuedGenerationPosSet_.erase(missingPos);
    }
    switch (result.kind) {
    case RetrievalResult::Kind::Success:
        break;
    case RetrievalResult::Kind::Split:
        sec::forEachChild(missingPos, [&](sec::Pos childPos) { tryQueuePosForRetrieval(childPos); });
        break;
    case RetrievalResult::Kind::Rejected:
    case RetrievalResult::Kind::Failed: {
        // gen task failed / cancelled (Java exceptionally): requeue so we can try again in the future
        std::lock_guard lock(retrievalMutex_);
        missingGenerationPosSet_.insert(missingPos);
        break;
    }
    }
}

void LodQuadTree::onGeneratorPlanChanged(bool generationEnabled) {
    // the generator plan was changed: clear the queues to make sure we get the correct sized generator tasks
    {
        std::lock_guard lock(retrievalMutex_);
        missingGenerationPosSet_.clear();
        queuedGenerationPosSet_.clear();
    }
    // don't allow regeneration until the next world gen tick
    canRegenerate_.store(false);

    // world gen tasks will need to be re-queued since all the render sections will already have been loaded
    requeueAllRetrievalTasks_.store(generationEnabled);
}

void LodQuadTree::tryQueuePosForRetrieval(sec::Pos pos) {
    if (!deps_.positionsToRetrieve) {
        return; // Java base provider: getPositionsToRetrieve == null
    }
    const std::optional<std::vector<sec::Pos>> missingPosList = deps_.positionsToRetrieve(pos, sec::detail(pos));
    if (!missingPosList.has_value()) {
        return;
    }

    std::lock_guard lock(retrievalMutex_);
    for (const sec::Pos missingPos : *missingPosList) {
        if (!queuedGenerationPosSet_.contains(missingPos)) {
            missingGenerationPosSet_.insert(missingPos);
        }
    }
}

bool LodQuadTree::isPosInWorldGenRange(
    sec::Pos     requestedPos,
    std::int32_t centerChunkX,
    std::int32_t centerChunkZ,
    std::int32_t maxChunkRadius
) {
    if (maxChunkRadius <= 0) {
        return true;
    }
    const BlockPos     centerBlockPos          = ChunkPos(centerChunkX, centerChunkZ).centerBlockPos();
    const std::int32_t blockDistanceFromCenter = sec::chebyshevSignedDistance(requestedPos, centerBlockPos);
    const std::int32_t maxBlockRadius          = wmul(maxChunkRadius, lod::CHUNK_WIDTH);
    return blockDistanceFromCenter <= maxBlockRadius;
}

std::size_t LodQuadTree::missingRetrievalCount() const {
    std::lock_guard lock(retrievalMutex_);
    return missingGenerationPosSet_.size();
}

std::size_t LodQuadTree::queuedRetrievalCount() const {
    std::lock_guard lock(retrievalMutex_);
    return queuedGenerationPosSet_.size();
}

//==========================//
// external render requests //
//==========================//

void LodQuadTree::clearRenderDataCache(bool clearExistingData) {
    std::lock_guard lock(treeTickMutex_);

    NodeIterator it = nodeIterator();
    while (it.hasNext()) {
        Node* quadNode = it.next();
        if (quadNode->value.has_value() && *quadNode->value) {
            if (clearExistingData) {
                // delete the old node so it's hard reloaded
                (*quadNode->value)->dispose();
                quadNode->value.reset();
            } else {
                // mark the node for re-loading
                (*quadNode->value)->renderDataDirty.store(true);
            }
        }
    }
    log::info("Render data cleared, please wait a moment for everything to reload...");
}

void LodQuadTree::queueReload(sec::Pos pos) {
    std::lock_guard lock(sectionsToReload_->mutex);
    sectionsToReload_->positions.push_back(pos);
    // the adjacent locations also need updating so lighting and water update correctly over LOD borders
    for (Direction direction : kCardinalDirections) {
        sectionsToReload_->positions.push_back(sec::adjacent(pos, direction));
    }
}

void LodQuadTree::attachToProvider(SourceProvider& provider) {
    if (attachedProvider_ != nullptr) {
        return;
    }
    attachedProvider_ = &provider;
    // Captures the queue, not the tree: the listener runs under the updater's position lock and must only
    // enqueue (never touch the provider); and it may still fire once after removeListener().
    std::shared_ptr<ReloadQueue> queue = sectionsToReload_;
    providerListener_                  = provider.addUpdateListener([queue](std::uint64_t pos) {
        std::lock_guard lock(queue->mutex);
        queue->positions.push_back(pos);
        for (Direction direction : kCardinalDirections) {
            queue->positions.push_back(sec::adjacent(pos, direction));
        }
    });
}

//==============//
// base methods //
//==============//

void LodQuadTree::close() {
    alive_->store(false);

    if (attachedProvider_ != nullptr && providerListener_.has_value()) {
        attachedProvider_->removeUpdateListener(*providerListener_);
        providerListener_.reset();
        attachedProvider_ = nullptr;
    }

    std::lock_guard lock(treeTickMutex_);
    NodeIterator    it = nodeIterator();
    while (it.hasNext()) {
        Node* quadNode = it.next();
        if (quadNode->value.has_value() && *quadNode->value) {
            (*quadNode->value)->dispose();
            quadNode->value.reset();
        }
    }
}

} // namespace dh::tree
