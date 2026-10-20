#pragma once

#include <string>

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

/// Parses a config key name (see dh/config/KeyNames.h) and logs one warning for an invalid name or a key that
/// Minecraft itself uses (F1, F2, F3, F5, F11). Returns the Windows VK code, 0 for "none" / invalid.
/// `optionPath` is only used in the warning text.
int resolveKey(std::string const& name, char const* optionPath);

} // namespace dh::input
