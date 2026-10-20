#pragma once

// Key names used in config.json ("client.keys.toggleRendering", "client.debug.hudKey") -> Windows virtual-key
// codes. The core has no windows.h, so the VK numbers are hardcoded here.
//
// Accepted names (case-insensitive): "none" or "" (no key), "f1".."f24", "numpad0".."numpad9",
// "numpad-decimal", "numpad-divide", "numpad-multiply", "numpad-subtract", "numpad-add", "a".."z", "0".."9",
// "insert", "delete", "home", "end", "pageup", "pagedown", "pause", "scrolllock".

#include <string_view>

namespace dh::config {

/// Windows VK code for `name`; 0 for "none", "" and unknown names (use isValidKeyName to tell them apart).
int parseKeyName(std::string_view name);

/// True for "none", "" and every accepted key name.
bool isValidKeyName(std::string_view name);

/// True for the keys Minecraft Bedrock uses itself: F1, F2, F3, F5, F11.
bool isVanillaKey(int vk);

} // namespace dh::config
