#pragma once

#include <cstdint>
#include <string>

namespace dh::hud {

/// Generic key/value registry shown in the debug HUD and dumped by F8 (`hud-dump: {...}` in dh.log).
void set(std::string const& key, std::string const& value);
void set(std::string const& key, double value);
void set(std::string const& key, std::int64_t value);
inline void set(std::string const& key, int value) { set(key, static_cast<std::int64_t>(value)); }
inline void set(std::string const& key, char const* value) { set(key, std::string{value}); }

/// JSON object of all registry entries.
std::string dumpJson();

/// Smoothed frame time in ms (EMA over HUD frames).
double frameMs();

void install();
void uninstall();

} // namespace dh::hud
