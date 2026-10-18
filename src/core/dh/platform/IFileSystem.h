#pragma once

// Minimal file-system seam. The core touches files only for config and
// (through ILodStore implementations) storage; everything else is the glue's
// business. Paths are std::filesystem::path so Windows wide paths survive.
//
// No exceptions: StdFileSystem uses the std::error_code overloads and C stdio.

#include "dh/util/Result.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace dh {

struct IFileSystem {
    virtual ~IFileSystem() = default;

    /// Whole-file read. NotFound / Io errors.
    virtual Result<std::vector<std::byte>> readFile(const std::filesystem::path& path) = 0;

    /// Writes to a temporary sibling then renames over `path`, so readers never
    /// observe a partial file. Parent directory must exist (see mkdirs).
    virtual Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) = 0;

    virtual bool exists(const std::filesystem::path& path) = 0;

    /// Creates `path` and all missing parents; success if it already exists as a directory.
    virtual Status mkdirs(const std::filesystem::path& path) = 0;

    /// Direct children of `path` (files and directories), sorted lexicographically
    /// by native path so results are deterministic across platforms.
    virtual Result<std::vector<std::filesystem::path>> listDir(const std::filesystem::path& path) = 0;

    /// Deletes a single file (not a directory). Success if it did not exist.
    virtual Status removeFile(const std::filesystem::path& path) = 0;
};

/// Real implementation over <filesystem> + C stdio.
class StdFileSystem final : public IFileSystem {
public:
    Result<std::vector<std::byte>> readFile(const std::filesystem::path& path) override;
    Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) override;
    bool   exists(const std::filesystem::path& path) override;
    Status mkdirs(const std::filesystem::path& path) override;
    Result<std::vector<std::filesystem::path>> listDir(const std::filesystem::path& path) override;
    Status                                     removeFile(const std::filesystem::path& path) override;
};

} // namespace dh
