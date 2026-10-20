#pragma once

// Visibility test shared by LodRenderer (terrain sections) and CloudRenderer (cloud tiles).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <cmath>

namespace dh::render {

/// Visibility planes in the "draw space" the section translate lands in. Five planes only: left, right, bottom, top and
/// "in front of the camera" (clip w > 0). Near/far depth planes are intentionally not used: their convention
/// (reversed-Z, [0,1] vs [-1,1]) is unverified (R3) and LODs have no meaningful far plane.
struct Frustum5 {
    bool      valid = false;
    glm::vec4 p[5]{};

    /// `transposed` = row-vector layout as detected by spikes::analyzeProjection (clip = v * M); see the derivation in
    /// docs/spikes/stage-a-render.md.
    static Frustum5 build(glm::mat4 const& proj, glm::mat4 const& view, bool transposed) {
        Frustum5  f;
        glm::mat4 m   = transposed ? view * proj : proj * view;
        auto      row = [&](int i) { return transposed ? m[i] : glm::row(m, i); };
        glm::vec4 r0 = row(0), r1 = row(1), r3 = row(3);
        f.p[0]  = r3 + r0;
        f.p[1]  = r3 - r0;
        f.p[2]  = r3 + r1;
        f.p[3]  = r3 - r1;
        f.p[4]  = r3;
        f.valid = true;
        for (auto const& pl : f.p)
            if (!std::isfinite(pl.x + pl.y + pl.z + pl.w)) f.valid = false;
        return f;
    }

    bool visible(glm::vec3 const& lo, glm::vec3 const& hi) const {
        for (auto const& pl : p) {
            glm::vec3 v{pl.x >= 0 ? hi.x : lo.x, pl.y >= 0 ? hi.y : lo.y, pl.z >= 0 ? hi.z : lo.z};
            if (pl.x * v.x + pl.y * v.y + pl.z * v.z + pl.w < 0) return false;
        }
        return true;
    }
};

} // namespace dh::render
