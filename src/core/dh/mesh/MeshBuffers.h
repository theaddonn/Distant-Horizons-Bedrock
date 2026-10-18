#pragma once

// MeshBuffers (opaque/transparent 16-byte-vertex buffers) is defined with the
// platform vertex-sink contract so the mesher and the sink share one type.
#include "dh/platform/IVertexSink.h"
