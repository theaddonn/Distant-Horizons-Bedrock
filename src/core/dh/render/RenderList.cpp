#include "dh/render/RenderList.h"

#include <algorithm>
#include <string>

namespace dh {

void RenderList::build(const tree::LodQuadTree& tree, const RenderListParams& params) {
    //====================================//
    // get and update the culling frustum //
    //====================================//

    bool           enableFrustumCulling;
    const Frustum* frustum;
    if (params.isShadowPass) {
        enableFrustumCulling =
            params.options == nullptr || !params.options->client.graphics.culling.disableShadowPassFrustumCulling;
        frustum = params.shadowFrustum; // by default the shadow pass has no frustum culling (NeverCullFrustum)
    } else {
        enableFrustumCulling =
            params.options == nullptr || !params.options->client.graphics.culling.disableFrustumCulling;
        frustum = &cameraFrustum_;
    }

    // update the frustum if necessary
    if (enableFrustumCulling && frustum == &cameraFrustum_) {
        const glm::mat4 worldViewProjection =
            viewProjectionMatrix(params.mcModelViewMatrix, params.exactCameraPosition, params.dhProjectionMatrix);
        cameraFrustum_.update(params.worldMinY, params.worldMaxY, worldViewProjection);
    }

    tree.collectEnabled(tempProcessNodeList_);
    build(
        tempProcessNodeList_,
        tree.getCenterBlockPos(),
        enableFrustumCulling ? frustum : nullptr,
        params.isShadowPass
    );
}

void RenderList::build(
    const std::vector<std::shared_ptr<tree::LodRenderSection>>& enabled,
    BlockPos2D                                                  sortCenter,
    const Frustum*                                              frustum,
    bool                                                        isShadowPass
) {
    // clear the old list so we can start fresh
    entries_.clear();

    int culled = 0;
    for (const std::shared_ptr<tree::LodRenderSection>& renderSection : enabled) {
        if (!renderSection) {
            continue;
        }

        const std::int32_t blockMinX   = sec::minCornerBlockX(renderSection->pos);
        const std::int32_t blockMinZ   = sec::minCornerBlockZ(renderSection->pos);
        const std::int32_t blockWidth  = sec::blockWidth(renderSection->pos);
        const std::int8_t  detailLevel = sec::detail(renderSection->pos);
        if (frustum != nullptr && !frustum->intersects(blockMinX, blockMinZ, blockWidth, detailLevel)) {
            culled++;
            continue;
        }

        // RenderBufferHandler.java:247-255: no buffer container or not enabled -> skip
        if (!renderSection->canRender() || !renderSection->getRenderingEnabled()) {
            continue;
        }

        RenderListEntry e;
        e.pos            = renderSection->pos;
        e.minBlockX      = blockMinX;
        e.minBlockZ      = blockMinZ;
        e.blockWidth     = blockWidth;
        e.detailLevel    = detailLevel;
        e.hasOpaque      = renderSection->hasOpaque();
        e.hasTransparent = renderSection->hasTransparent();
        e.section        = renderSection;
        entries_.push_back(std::move(e));
    }

    // sortBufferContainersNearToFar: manhattan distance of the section centre from the tree centre
    std::stable_sort(entries_.begin(), entries_.end(), [&](const RenderListEntry& a, const RenderListEntry& b) {
        const std::int32_t aDist = sec::centerBlockPos(a.pos).manhattanDist(sortCenter);
        const std::int32_t bDist = sec::centerBlockPos(b.pos).manhattanDist(sortCenter);
        return aDist < bDist;
    });

    if (isShadowPass) {
        shadowCulledBufferCount_  = culled;
        shadowVisibleBufferCount_ = static_cast<int>(entries_.size());
    } else {
        culledBufferCount_  = culled;
        visibleBufferCount_ = static_cast<int>(entries_.size());
    }
}

std::string RenderList::vboRenderDebugMenuString(bool frustumCullingDisabled) const {
    std::string countText = std::to_string(visibleBufferCount_);
    if (!frustumCullingDisabled) {
        countText += "/" + std::to_string(visibleBufferCount_ + culledBufferCount_);
    }
    return "VBO Render Count: [" + countText + "]";
}

std::string RenderList::shadowPassRenderDebugMenuString(bool frustumCullingDisabled) const {
    std::string countText = std::to_string(shadowVisibleBufferCount_);
    if (!frustumCullingDisabled) {
        countText += "/" + std::to_string(shadowVisibleBufferCount_ + shadowCulledBufferCount_);
    }
    return "Shadow VBO Render Count: [" + countText + "]";
}

} // namespace dh
