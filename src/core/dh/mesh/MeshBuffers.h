#pragma once

#include <cstddef>
#include <vector>

namespace dh {

/// Output of LodQuadBuilder::build(): vertex data in DH's 16-byte vertex format,
/// split into buffers of at most 2 MiB (multiple of 64 bytes = one quad).
/// Plain aggregate; may later be aliased by platform/IVertexSink.h.
struct MeshBuffers {
    std::vector<std::vector<std::byte>> opaque;
    std::vector<std::vector<std::byte>> transparent;
};

} // namespace dh
