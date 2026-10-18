#pragma once

namespace dh::input {

/// Rising-edge detector on a Windows virtual-key; true once per press, only while the game window is focused.
/// Call at most once per frame per key from a single place.
bool pressed(int vk);

} // namespace dh::input

namespace dh::input {

/// True while either Shift key is held (and the game window is focused).
bool shiftDown();

} // namespace dh::input

namespace dh::input {

/// True while either Ctrl key is held (and the game window is focused).
bool ctrlDown();

} // namespace dh::input
