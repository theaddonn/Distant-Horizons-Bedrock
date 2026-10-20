#pragma once

// Stage-A renderer wiring: hooks LevelRendererCamera::$render (draw after the vanilla world) and
// LevelRendererPlayer::setupCamera (only active in far-plane mode "zfar-override"), plus the key handler. The hooks
// are installed on enable but are passthrough until the LOD pipeline is on (client.enableRendering starts it with the
// world).
//
// Keys (config.json): client.keys.toggleRendering switches the pipeline on/off; with client.debug.enabled the numpad
// debug map is active (docs/DEBUG.md). Shift+F5 is the synthetic test feed, only with DH_TEST_FEED=1.

namespace dh::render {

class LodRenderer;

void install();
void uninstall();

/// The renderer instance (valid between install() and uninstall()); core wiring uploads through renderer().sink().
LodRenderer& renderer();

bool enabled();
/// Renderer draw on/off (the world driver sets it with the pipeline; the toggle key goes through ClientWorldDriver::setPipeline).
void setEnabled(bool on);

} // namespace dh::render
