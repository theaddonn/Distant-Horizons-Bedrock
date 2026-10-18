#pragma once

// Pure parts of core/util/RenderUtil.java: near clip plane (overdraw table, fast-movement reduction,
// FOV/aspect correction, altitude override), far clip plane and the projection clip-plane patch.
// Every Config / Minecraft wrapper read of the Java code is an explicit input here.

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "dh/pos/LodUtil.h"
#include "dh/util/Bits.h"
#include "dh/util/MathUtil.h"

namespace dh::renderutil {

/// RenderUtil.DynamicOverdraw (blocks per second).
inline constexpr float kDynamicOverdrawMinSpeed = 10.0f;  ///< a little faster than sprinting (7)
inline constexpr float kDynamicOverdrawMaxSpeed = 100.0f; ///< (float) LodUtil.MAX_SPECTATOR_SPEED_IN_BLOCKS_PER_SEC
inline constexpr float kDynamicOverdrawMinOverdrawRatio = 0.2f;

/// Everything getNearClipPlaneInBlocks() reads from Config / MC_RENDER / MC / ClientApi / IRIS_ACCESSOR.
struct NearClipInputs {
    /// Config.Client.Advanced.Graphics.Culling.overdrawPrevention (< 0 = automatic)
    float overdrawPreventionSetting = -1.0f;
    /// Config...Culling.reduceOverdrawWithFastMovement
    bool reduceOverdrawWithFastMovement = true;
    /// MC_RENDER.getRenderDistance() (chunks)
    std::int32_t vanillaChunkRenderDistance = 12;
    /// IRIS_ACCESSOR != null && isShaderPackInUse()
    bool shaderPackInUse = false;
    /// ClientApi.INSTANCE.getAvgCameraSpeed() (blocks/s)
    double avgCameraSpeedBlocksPerSec = 0.0;
    /// Config...Debugging.lodOnlyMode
    bool lodOnlyMode = false;
    /// MC.getPlayerBlockPos().getY()
    std::int32_t playerBlockY = 64;
    /// level.getMaxHeight() (-1 when no level is loaded: no altitude override)
    std::int32_t levelMaxHeight = 320;
    bool         levelLoaded    = true;
    /// MC_RENDER.getTargetFramebufferViewportWidth() / Height()
    std::int32_t viewportWidth  = 16;
    std::int32_t viewportHeight = 9;
};

/// RenderUtil.getHeightBasedNearClipOverrideBlockDistance: -1 if no override is necessary.
inline float
heightBasedNearClipOverrideBlockDistance(bool levelLoaded, std::int32_t playerBlockY, std::int32_t levelMaxHeight) {
    if (levelLoaded) {
        // if the player is a significant distance above the world, increase the near clip plane to fix Z imprecision
        if (playerBlockY > wadd(levelMaxHeight, 1000)) {
            return static_cast<float>(wsub(playerBlockY, wadd(levelMaxHeight, 1000)));
        }
    }
    return -1.0f;
}

/// First half of RenderUtil.getNearClipPlaneInBlocks: the overdraw fraction (auto table / clamp, then the
/// fast-movement reduction).
inline float overdrawPrevention(const NearClipInputs& in) {
    float overdraw = in.overdrawPreventionSetting;
    if (overdraw < 0) {
        // automatic mode: at low render distances this hides the vanilla RD border
        if (in.shaderPackInUse) {
            overdraw = 0.2f;
        } else if (in.vanillaChunkRenderDistance <= 2) {
            overdraw = 0.2f;
        } else if (in.vanillaChunkRenderDistance <= 4) {
            overdraw = 0.3f;
        } else if (in.vanillaChunkRenderDistance <= 6) {
            overdraw = 0.6f;
        } else if (in.vanillaChunkRenderDistance <= 10) {
            overdraw = 0.8f;
        } else {
            overdraw = 0.9f;
        }
    } else {
        // prevent an overdraw of 0 since that causes rendering issues
        overdraw = math::clamp(0.05f, overdraw, 1.0f);
    }

    if (in.reduceOverdrawWithFastMovement) {
        const double avgSpeed = in.avgCameraSpeedBlocksPerSec;
        if (avgSpeed >= static_cast<double>(kDynamicOverdrawMinSpeed)) {
            // convert the speed into a range of 0.0 - 1.0 (float - double -> double, then (float))
            float speedRange = static_cast<float>(
                (static_cast<double>(kDynamicOverdrawMaxSpeed) - avgSpeed)
                / static_cast<double>(kDynamicOverdrawMaxSpeed)
            );
            // without the max we could completely remove vanilla rendering at high speeds
            speedRange  = jmaxf(speedRange, kDynamicOverdrawMinOverdrawRatio);
            overdraw   *= speedRange;
        }
    }
    return overdraw;
}

/// RenderUtil.getNearClipPlaneDistanceInBlocks(overdrawPreventionPercent).
inline float nearClipPlaneDistanceInBlocks(float overdrawPreventionPercent, const NearClipInputs& in) {
    const std::int32_t vanillaBlockRenderedDistance = wmul(in.vanillaChunkRenderDistance, lod::CHUNK_WIDTH);

    float nearClipPlane;
    if (in.lodOnlyMode) {
        nearClipPlane = 0.5f;
    } else {
        nearClipPlane  = static_cast<float>(vanillaBlockRenderedDistance);
        nearClipPlane *= overdrawPreventionPercent;
        // never closer than 1 block, otherwise Z-fighting and other issues may occur
        if (nearClipPlane < 1.0f) {
            nearClipPlane = 1.0f;
        }
    }

    const float heightOverride =
        heightBasedNearClipOverrideBlockDistance(in.levelLoaded, in.playerBlockY, in.levelMaxHeight);
    if (heightOverride != -1.0f) {
        nearClipPlane = heightOverride;
    }

    // the FOV setting must not affect DH's near clip plane (zoom mods), so it is fixed at MC's default 70
    const double fov         = 70;
    const double aspectRatio = static_cast<double>(in.viewportWidth) / in.viewportHeight;

    // https://stackoverflow.com/questions/8101119 (methodically choosing the near clip plane)
    const double pi = 3.141592653589793; // Math.PI
    return static_cast<float>(
        static_cast<double>(nearClipPlane)
        / std::sqrt(1.0 + math::pow2(std::tan(fov / 180.0 * pi / 2.0)) * (math::pow2(aspectRatio) + 1.0))
    );
}

/// RenderUtil.getNearClipPlaneInBlocks().
inline float nearClipPlaneInBlocks(const NearClipInputs& in) {
    return nearClipPlaneDistanceInBlocks(overdrawPrevention(in), in);
}

/// The near clip distance setDhProjectionMatrix actually installs: when no altitude override applies the plane
/// is limited to 7.5 blocks (the discard shader handles everything farther away anyway).
inline float dhNearClipDistance(const NearClipInputs& in) {
    float nearClipDist = nearClipPlaneInBlocks(in);
    if (heightBasedNearClipOverrideBlockDistance(in.levelLoaded, in.playerBlockY, in.levelMaxHeight) == -1) {
        nearClipDist = jminf(nearClipDist, 7.5f);
    }
    return nearClipDist;
}

/// RenderUtil.getFarClipPlaneDistanceInBlocks. Without Iris: (lodChunkRadius * 16 + 512) * 2 (int maths, the
/// * 2 prevents clipping when high above the world). With Iris: (lodBlockDist + 512) * sqrt(2) as float.
inline float farClipPlaneDistanceInBlocks(std::int32_t lodChunkRenderDistanceRadius, bool irisPresent = false) {
    const std::int32_t lodBlockDist = wmul(lodChunkRenderDistanceRadius, lod::CHUNK_WIDTH);
    if (irisPresent) {
        return static_cast<float>(static_cast<double>(wadd(lodBlockDist, 512)) * std::sqrt(2.0));
    }
    return static_cast<float>(wmul(wadd(lodBlockDist, lod::REGION_WIDTH), 2));
}

/// RenderUtil.setClipPlanes: patches the projection's m22 / m23 (DhApiMat4f row-major names; glm m[2][2] and
/// m[3][2]) with the JOML perspective formulas Minecraft uses. Reverse-Z callers pass (far, near, true).
inline void setClipPlanes(glm::mat4& matrix, float nearClip, float farClip, bool zZeroToOne) {
    matrix[2][2] = (zZeroToOne ? farClip : farClip + nearClip) / (nearClip - farClip);
    matrix[3][2] = (zZeroToOne ? farClip : farClip + farClip) * nearClip / (nearClip - farClip);
}

} // namespace dh::renderutil
