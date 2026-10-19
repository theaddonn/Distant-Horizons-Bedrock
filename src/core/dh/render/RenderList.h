#pragma once

// Port of render/RenderBufferHandler.java: tells the renderer which sections to draw this frame.
//
// build() takes the tree's enabled sections (LodQuadTree::collectEnabled), frustum-culls them
// (DhFrustumBounds / NeverCullFrustum, Config disableFrustumCulling / disableShadowPassFrustumCulling) and
// hands back the renderable ones sorted near-to-far by manhattan distance from the tree centre
// (RenderBufferHandler.sortBufferContainersNearToFar, :106-116).
//
// Deviation: Java's SortedArraySet never applied that comparator (its add() only deduplicates and appends), so
// the Java render order was the HashSet order of the tick's enable set. The near-to-far order is the documented
// intent (translucency sorting, reddit link at :128) and is what this port produces, with a stable sort so
// equal distances keep the tick's deterministic walk order.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "dh/config/Options.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/pos/BlockPos.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/Frustum.h"
#include "dh/tree/LodQuadTree.h"
#include "dh/tree/LodRenderSection.h"

namespace dh {

struct RenderListEntry {
    sec::Pos                                pos       = 0;
    std::int32_t                            minBlockX = 0, minBlockZ = 0; ///< section origin (min corner block)
    std::int32_t                            blockWidth  = 0;
    std::int8_t                             detailLevel = 0;
    bool                                    hasOpaque = false, hasTransparent = false;
    std::shared_ptr<tree::LodRenderSection> section;
};

/// Java RenderParam (the fields buildRenderList reads).
struct RenderListParams {
    /// Config.Client.Advanced.Graphics.Culling (disableFrustumCulling / disableShadowPassFrustumCulling).
    const Options* options = nullptr;
    /// IRIS_ACCESSOR.isRenderingShadowPass()
    bool isShadowPass = false;
    /// renderParams.clientLevelWrapper min/max height (max is `minHeight + height` as Java passes it)
    std::int32_t worldMinY = -64;
    std::int32_t worldMaxY = 320;
    /// renderParams.mcModelViewMatrix / exactCameraPosition / dhProjectionMatrix (Frustum.h glm layout)
    glm::mat4  mcModelViewMatrix{1.0f};
    glm::dvec3 exactCameraPosition{0.0};
    glm::mat4  dhProjectionMatrix{1.0f};
    /// IDhApiShadowCullingFrustum override; nullptr = NeverCullFrustum (Java default).
    const Frustum* shadowFrustum = nullptr;
    /// Platform-computed world-space view-projection (JOML/column-vector layout, m[c][r]; world coords in, clip
    /// out). When set it replaces viewProjectionMatrix(mcModelViewMatrix, exactCameraPosition, dhProjectionMatrix):
    /// Bedrock's matrices are not Minecraft-Java's, so the platform composes the frustum matrix with the convention
    /// its own draw path has validated instead of re-deriving it from Java's layout assumptions.
    bool      hasWorldViewProjection = false;
    glm::mat4 worldViewProjection{1.0f};
};

class RenderList {
public:
    /// RenderBufferHandler.buildRenderList. Updates the camera frustum from the params when culling is on.
    void build(const tree::LodQuadTree& tree, const RenderListParams& params);

    /// Culls `enabled` with an already-updated frustum (nullptr = no culling). Exposed for tests / custom frustums.
    void build(
        const std::vector<std::shared_ptr<tree::LodRenderSection>>& enabled,
        BlockPos2D                                                  sortCenter,
        const Frustum*                                              frustum,
        bool                                                        isShadowPass
    );

    const std::vector<RenderListEntry>& entries() const { return entries_; }
    const Frustum&                      cameraFrustum() const { return cameraFrustum_; }

    int visibleBufferCount() const { return visibleBufferCount_; }
    int culledBufferCount() const { return culledBufferCount_; }
    /// Diagnostics for the last camera-pass build: sections the tree enabled, and enabled sections that survived
    /// the frustum but had no GPU buffers yet (canRender() false) or were not rendering-enabled.
    int enabledCount() const { return enabledCount_; }
    int notRenderableCount() const { return notRenderableCount_; }
    int shadowVisibleBufferCount() const { return shadowVisibleBufferCount_; }
    int shadowCulledBufferCount() const { return shadowCulledBufferCount_; }

    /// "VBO Render Count: [visible/total]" (total only when frustum culling is enabled).
    std::string vboRenderDebugMenuString(bool frustumCullingDisabled) const;
    std::string shadowPassRenderDebugMenuString(bool frustumCullingDisabled) const;

private:
    Frustum                                              cameraFrustum_;
    std::vector<RenderListEntry>                         entries_;
    std::vector<std::shared_ptr<tree::LodRenderSection>> tempProcessNodeList_;

    int visibleBufferCount_       = 0;
    int culledBufferCount_        = 0;
    int enabledCount_             = 0;
    int notRenderableCount_       = 0;
    int shadowVisibleBufferCount_ = 0;
    int shadowCulledBufferCount_  = 0;
};

} // namespace dh
