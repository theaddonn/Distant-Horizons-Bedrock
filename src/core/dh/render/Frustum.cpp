#include "dh/render/Frustum.h"

#include <cmath>
#include <limits>

#include "dh/util/Bits.h"

namespace dh {

namespace {

/// JOML Math.invsqrt(float): 1.0f / (float) java.lang.Math.sqrt((double) r).
float jomlInvsqrt(float r) { return 1.0f / static_cast<float>(std::sqrt(static_cast<double>(r))); }

/// One plane of FrustumIntersection.set(m, allowTestSpheres = true): (m.m03 +/- m.m0k, m.m13 +/- m.m1k, ...)
/// then normalised by the xyz length. JOML m<c><r> == glm m[c][r].
Frustum::Plane extractPlane(const glm::mat4& m, int k, bool negative) {
    Frustum::Plane p;
    if (negative) {
        p.x = m[0][3] + m[0][k];
        p.y = m[1][3] + m[1][k];
        p.z = m[2][3] + m[2][k];
        p.w = m[3][3] + m[3][k];
    } else {
        p.x = m[0][3] - m[0][k];
        p.y = m[1][3] - m[1][k];
        p.z = m[2][3] - m[2][k];
        p.w = m[3][3] - m[3][k];
    }
    const float invl  = jomlInvsqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    p.x              *= invl;
    p.y              *= invl;
    p.z              *= invl;
    p.w              *= invl;
    return p;
}

/// JOML Matrix4f.frustumAabb on an (inverted view-projection) matrix: the 8 NDC corners un-projected.
void frustumAabb(const glm::mat4& inv, glm::vec3& outMin, glm::vec3& outMax) {
    float minX = std::numeric_limits<float>::infinity(), minY = minX, minZ = minX;
    float maxX = -std::numeric_limits<float>::infinity(), maxY = maxX, maxZ = maxX;
    for (int t = 0; t < 8; t++) {
        const float x    = static_cast<float>((t & 1) << 1) - 1.0f;
        const float y    = static_cast<float>(((t >> 1) & 1) << 1) - 1.0f;
        const float z    = static_cast<float>(((t >> 2) & 1) << 1) - 1.0f;
        const float invW = 1.0f / (inv[0][3] * x + inv[1][3] * y + inv[2][3] * z + inv[3][3]);
        const float nx   = (inv[0][0] * x + inv[1][0] * y + inv[2][0] * z + inv[3][0]) * invW;
        const float ny   = (inv[0][1] * x + inv[1][1] * y + inv[2][1] * z + inv[3][1]) * invW;
        const float nz   = (inv[0][2] * x + inv[1][2] * y + inv[2][2] * z + inv[3][2]) * invW;
        minX             = minX < nx ? minX : nx;
        minY             = minY < ny ? minY : ny;
        minZ             = minZ < nz ? minZ : nz;
        maxX             = maxX > nx ? maxX : nx;
        maxY             = maxY > ny ? maxY : ny;
        maxZ             = maxZ > nz ? maxZ : nz;
    }
    outMin = glm::vec3(minX, minY, minZ);
    outMax = glm::vec3(maxX, maxY, maxZ);
}

/// JOML testAab term: n * (n < 0 ? min : max). Written out so the float evaluation order is Java's.
inline float pTerm(float n, float minV, float maxV) { return n * (n < 0 ? minV : maxV); }

inline bool planeTest(const Frustum::Plane& p, float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
    const float s = pTerm(p.x, minX, maxX) + pTerm(p.y, minY, maxY) + pTerm(p.z, minZ, maxZ);
    return s >= -p.w;
}

} // namespace

void Frustum::update(std::int32_t worldMinBlockY, std::int32_t worldMaxBlockY, const glm::mat4& m) {
    worldMinY_ = static_cast<float>(worldMinBlockY);
    worldMaxY_ = static_cast<float>(worldMaxBlockY);

    // FrustumIntersection.set(Matrix4fc): nx = col3 + col0 ... pz = col3 - col2 (JOML accessor terms)
    planes_[NX] = extractPlane(m, 0, true);
    planes_[PX] = extractPlane(m, 0, false);
    planes_[NY] = extractPlane(m, 1, true);
    planes_[PY] = extractPlane(m, 1, false);
    planes_[NZ] = extractPlane(m, 2, true);
    planes_[PZ] = extractPlane(m, 2, false);

    // new Matrix4f(worldViewProjection).invert().frustumAabb(boundsMin, boundsMax)
    frustumAabb(glm::inverse(m), boundsMin_, boundsMax_);
}

bool Frustum::intersects(
    std::int32_t lodBlockPosMinX,
    std::int32_t lodBlockPosMinZ,
    std::int32_t lodBlockWidth,
    std::int32_t /*lodDetailLevel*/
) const {
    // Vector3f lodMin(minX, worldMinY, minZ); lodMax(minX + width, worldMaxY, minZ + width)
    const float minX = static_cast<float>(lodBlockPosMinX);
    const float minZ = static_cast<float>(lodBlockPosMinZ);
    const float maxX = static_cast<float>(wadd(lodBlockPosMinX, lodBlockWidth));
    const float maxZ = static_cast<float>(wadd(lodBlockPosMinZ, lodBlockWidth));
    return testAab(minX, worldMinY_, minZ, maxX, worldMaxY_, maxZ);
}

bool Frustum::testAab(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) const {
    return planeTest(planes_[NX], minX, minY, minZ, maxX, maxY, maxZ)
        && planeTest(planes_[PX], minX, minY, minZ, maxX, maxY, maxZ)
        && planeTest(planes_[NY], minX, minY, minZ, maxX, maxY, maxZ)
        && planeTest(planes_[PY], minX, minY, minZ, maxX, maxY, maxZ)
        && planeTest(planes_[NZ], minX, minY, minZ, maxX, maxY, maxZ)
        && planeTest(planes_[PZ], minX, minY, minZ, maxX, maxY, maxZ);
}

glm::mat4
viewProjectionMatrix(const glm::mat4& modelView, const glm::dvec3& exactCameraPosition, const glm::mat4& projection) {
    // WORLD_VIEW_JOML_MATRIX.setTransposed(mcModelView).translate(-x, -y, -z): JOML translateGeneric
    //   dest.m3r = m0r * x + (m1r * y + (m2r * z + m3r))
    const float x = -static_cast<float>(exactCameraPosition.x);
    const float y = -static_cast<float>(exactCameraPosition.y);
    const float z = -static_cast<float>(exactCameraPosition.z);

    glm::mat4 view = modelView;
    for (int r = 0; r < 4; r++) {
        view[3][r] = modelView[0][r] * x + (modelView[1][r] * y + (modelView[2][r] * z + modelView[3][r]));
    }

    // WORLD_VIEW_PROJ_JOML_MATRIX.setTransposed(dhProjection).mul(worldView): JOML mulGeneric
    //   dest.m<c><r> = l.m0r * r.m<c>0 + (l.m1r * r.m<c>1 + (l.m2r * r.m<c>2 + l.m3r * r.m<c>3))
    glm::mat4 out(0.0f);
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[c][r] =
                projection[0][r] * view[c][0]
                + (projection[1][r] * view[c][1] + (projection[2][r] * view[c][2] + projection[3][r] * view[c][3]));
        }
    }
    return out;
}

} // namespace dh
