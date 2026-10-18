#pragma once

// Bedrock-side platform bits shared by the client and server DLLs.
//
// dh::SystemClock and dh::StdFileSystem (core) are used as-is. There is no
// LeviExecutor: LeviLamina 26.51.6 only ships ServerThreadExecutor (server
// only) and ThreadPoolExecutor, and the core's ThreadPools already provides
// the worker pools. A render-thread (game-thread) IExecutor is client-only and
// belongs with the client renderer glue.

#include <filesystem>
#include <string>

namespace dh::bedrock {

struct ModPaths {
    std::filesystem::path configDir; ///< NativeMod::getConfigDir()
    std::filesystem::path dataDir;   ///< NativeMod::getDataDir()

    /// `<dataDir>/worlds/<sanitized levelName>`; the LOD database lives under it. Not created.
    std::filesystem::path worldDataDir(const std::string& levelName) const;

    /// Resolves from NativeMod::current(); empty paths if no mod context yet.
    static ModPaths fromCurrentMod();
};

/// Replaces characters illegal in Windows file names (and control chars) with '_'; never empty.
std::string sanitizePathComponent(const std::string& name);

} // namespace dh::bedrock
