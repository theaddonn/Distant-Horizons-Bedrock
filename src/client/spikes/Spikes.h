#pragma once

// Render spikes R0-R3 (see docs/spikes/render-spikes.md). Hooks are installed on enable but every spike is off until
// toggled with F9-F12:
//   F9  R0 inventory dump to dh.log           Shift+F10  cycle origin mode (camera-relative / absolute)
//   F10 R1 quad (+ R2 extra quads) on/off     Shift+F11  cycle primitive mode (QuadList / TriangleList)
//   F11 R1b fogged far quad on/off
//   F12 R3 far-plane mode: off -> local projection push -> mZFar override

namespace dh::spikes {

void install();
void uninstall();

} // namespace dh::spikes
