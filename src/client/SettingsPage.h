#pragma once

// The in-game settings pages (Settings -> Mods -> Distant Horizons), built with Lever from the option metadata table
// (dh/config/Options.h): one widget per option, grouped into a few pages. Edits go through the ConfigBridge store,
// which saves config.json and runs the change listeners (RenderHooks applies the rendering options live).

namespace dh::settings {

/// Registers the pages with Lever. Call after ConfigBridge::init and after the render hooks are installed.
void install();

/// Drops the registration (the callbacks point into this DLL). Call from disable().
void uninstall();

/// Makes an open page re-read one option after it changed outside the UI (a keybind, a reset); `optionPath` is the
/// dotted path from the metadata table, e.g. "client.enableRendering". Game thread only.
void notifyChanged(const char* optionPath);

} // namespace dh::settings
