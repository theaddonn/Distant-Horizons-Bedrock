#pragma once

// Stage-A renderer wiring: hooks LevelRendererCamera::$render (draw after the vanilla world) and
// LevelRendererPlayer::setupCamera (only active in far-plane mode "zfar-override"), plus the key handler. The hooks
// are installed on enable but are passthrough until F5 turns the renderer on (off at start).
//
//   F5 renderer on/off          Shift+F5 test feed on/off
//   F4 primitive/winding cycle  Shift+F4 far-plane mode cycle   Ctrl+F4 near cut-off on/off
//   F6 material cycle           Shift+F6 origin mode            Ctrl+F6 transparent pass on/off
//
// Coexistence with the render spikes (src/client/spikes): both hook the same functions; the hook chain runs both, they
// do not interfere as long as the spike toggles (F10-F12) are off. Leave them off while testing Stage A.

namespace dh::render {

class LodRenderer;

void install();
void uninstall();

/// The renderer instance (valid between install() and uninstall()); core wiring uploads through renderer().sink().
LodRenderer& renderer();

bool enabled();

} // namespace dh::render
