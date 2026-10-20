#pragma once

#ifndef DH_GIT_SHA
#define DH_GIT_SHA "unknown"
#endif

// Injected by xmake.lua (DH_VERSION); the fallback must equal it (scripts/check-version.sh).
#ifndef DH_MOD_VERSION
#define DH_MOD_VERSION "0.1.0"
#endif

namespace dh {

inline constexpr char const* kModVersion = DH_MOD_VERSION;
inline constexpr char const* kGitSha     = DH_GIT_SHA;

} // namespace dh
