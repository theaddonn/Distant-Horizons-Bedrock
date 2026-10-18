#pragma once

// Synthetic MeshBuffers pushed through MeshUploader::upload so the full IVertexSink -> mce::Mesh -> draw path can be
// seen in game before the real pipeline (G2) exists. Toggled with Shift+F5.

#include <glm/glm.hpp>

namespace dh::render {

class LodRenderer;

namespace testfeed {

/// Builds the feed around `playerPos` and uploads it, or removes it again if it is already active.
void toggle(LodRenderer& renderer, glm::dvec3 const& playerPos);

bool active();

/// Forgets the active state without removing anything (level exit: the uploader is cleared separately).
void reset();

} // namespace testfeed
} // namespace dh::render
