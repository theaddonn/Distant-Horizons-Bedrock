#pragma once

#include <cstdint>
#include <string>

namespace dh::hud {

/// Generic key/value registry shown in the debug overlay and dumped by Numpad . (`hud-dump: {...}` in dh.log).
/// The registry works regardless of the overlay.
void set(std::string const& key, std::string const& value);
void set(std::string const& key, double value);
void set(std::string const& key, std::int64_t value);
inline void set(std::string const& key, int value) { set(key, static_cast<std::int64_t>(value)); }
inline void set(std::string const& key, char const* value) { set(key, std::string{value}); }

/// JSON object of all registry entries.
std::string dumpJson();

/// Smoothed frame time in ms (EMA over overlay frames; 0 while the overlay is not installed).
double frameMs();

/// Installs the overlay draw listener, only when client.debug.enabled (ConfigBridge must be initialised).
void install();
void uninstall();

} // namespace dh::hud
