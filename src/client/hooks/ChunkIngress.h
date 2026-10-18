#pragma once

// Smoke hook: Level::onChunkLoaded -> BedrockChunk -> dh::createFromChunk (Shift+F7 toggles, off by default).
// See docs/spikes/chunk-ingress.md.

namespace dh::ingress {

void install();
void uninstall();

} // namespace dh::ingress
