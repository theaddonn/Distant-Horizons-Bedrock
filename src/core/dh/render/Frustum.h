#pragma once

// Port of render/renderer/cullingFrustum/DhFrustumBounds.java together with the JOML 1.10.2 maths it
// delegates to (FrustumIntersection.set(Matrix4fc) plane extraction + normalisation, testAab, and
// Matrix4f.frustumAabb), plus NeverCullFrustum.
//
// Matrix convention: a glm::mat4 `m` is column-major, so m[c][r] is row r / column c of the maths matrix,
// i.e. DhApiMat4f.m<r><c> (= DH's row-major array mat[r*4+c]). DH transposes that array into JOML
// (DhMat4f.createJomlMatrix), whose m<c><r> accessor therefore reads the same element: JOML m03 == m[0][3].
// The plane extraction below is written against the glm layout so the two agree element for element.
//
// All plane maths is IEEE single precision like Java's (Math.fma is not used: JOML's Math.fma falls back to
// a*b+c unless joml.useMathFma is set, and -ffp-contract=off keeps clang from fusing it either).

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace dh {

class Frustum {
public:
    struct Plane {
        float x = 0, y = 0, z = 0, w = 0;
    };
    /// JOML FrustumIntersection field order (nx, px, ny, py, nz, pz).
    enum PlaneIndex : int { NX = 0, PX = 1, NY = 2, PY = 3, NZ = 4, PZ = 5 };

    /// DhFrustumBounds.update(worldMinBlockY, worldMaxBlockY, dhWorldViewProjection).
    void update(std::int32_t worldMinBlockY, std::int32_t worldMaxBlockY, const glm::mat4& worldViewProjection);

    /// DhFrustumBounds.intersects: AABB [minX, minX+width) x [worldMinY, worldMaxY] x [minZ, minZ+width).
    /// The int sums wrap like Java before the float conversion.
    bool intersects(
        std::int32_t lodBlockPosMinX,
        std::int32_t lodBlockPosMinZ,
        std::int32_t lodBlockWidth,
        std::int32_t lodDetailLevel
    ) const;

    /// JOML FrustumIntersection.testAab (6 planes, p-vertex test, `>= -w`).
    bool testAab(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) const;

    const std::array<Plane, 6>& planes() const { return planes_; }
    float                       worldMinY() const { return worldMinY_; }
    float                       worldMaxY() const { return worldMaxY_; }

    /// invert(worldViewProjection).frustumAabb(): the world-space AABB of the frustum. DhFrustumBounds computes
    /// it but never reads it; kept for debugging. NOT bit-exact with JOML (the inverse comes from glm, whose
    /// cofactor association differs); the corner projection itself follows JOML.
    const glm::vec3& boundsMin() const { return boundsMin_; }
    const glm::vec3& boundsMax() const { return boundsMax_; }

private:
    std::array<Plane, 6> planes_{};
    float                worldMinY_ = 0, worldMaxY_ = 0;
    glm::vec3            boundsMin_{0}, boundsMax_{0};
};

/// RenderBufferHandler: WORLD_VIEW_JOML_MATRIX = transpose(mcModelView).translate(-cam);
/// WORLD_VIEW_PROJ = transpose(dhProjection).mul(worldView). Reproduces JOML's float operation order
/// (translateGeneric / mulGeneric without fma). Inputs/outputs use the glm layout described above.
glm::mat4
viewProjectionMatrix(const glm::mat4& modelView, const glm::dvec3& exactCameraPosition, const glm::mat4& projection);

} // namespace dh
