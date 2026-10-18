#include "dh/platform/IFileSystem.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>

namespace dh {

namespace {

namespace fs = std::filesystem;

std::string pathForMessage(const fs::path& path) { return path.string(); }

std::FILE* openFile(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    std::FILE*   file = nullptr;
    if (_wfopen_s(&file, path.c_str(), wmode.c_str()) != 0) return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

Error ioError(std::string_view what, const fs::path& path, const std::error_code& ec) {
    std::string msg(what);
    msg += " '";
    msg += pathForMessage(path);
    msg += "'";
    if (ec) {
        msg += ": ";
        msg += ec.message();
    }
    return Error{ErrorCode::Io, std::move(msg)};
}

} // namespace

Result<std::vector<std::byte>> StdFileSystem::readFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return Error{ErrorCode::NotFound, "file not found '" + pathForMessage(path) + "'"};

    std::FILE* file = openFile(path, "rb");
    if (!file) return ioError("cannot open", path, {});

    std::vector<std::byte> data;
    std::byte              chunk[64 * 1024];
    for (;;) {
        std::size_t n = std::fread(chunk, 1, sizeof(chunk), file);
        if (n > 0) data.insert(data.end(), chunk, chunk + n);
        if (n < sizeof(chunk)) break;
    }
    bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return ioError("read failed", path, {});
    return data;
}

Status StdFileSystem::writeFileAtomic(const fs::path& path, std::span<const std::byte> data) {
    fs::path tmp  = path;
    tmp          += ".tmp";

    std::FILE* file = openFile(tmp, "wb");
    if (!file) return ioError("cannot create", tmp, {});

    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
    ok      = (std::fflush(file) == 0) && ok;
    ok      = (std::fclose(file) == 0) && ok;
    if (!ok) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return ioError("write failed", tmp, {});
    }

    std::error_code ec;
    fs::rename(tmp, path, ec); // replaces an existing target on both POSIX and Windows
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return ioError("rename failed", path, ec);
    }
    return Status::ok();
}

bool StdFileSystem::exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

Status StdFileSystem::mkdirs(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec); // no-op (and no error) when it already exists
    if (ec) return ioError("mkdirs failed", path, ec);
    if (!fs::is_directory(path, ec)) return ioError("not a directory", path, ec);
    return Status::ok();
}

Result<std::vector<fs::path>> StdFileSystem::listDir(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec))
        return Error{ErrorCode::NotFound, "not a directory '" + pathForMessage(path) + "'"};

    std::vector<fs::path>  entries;
    fs::directory_iterator it(path, ec);
    if (ec) return ioError("listDir failed", path, ec);
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) return ioError("listDir failed", path, ec);
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
        return a.native() < b.native();
    });
    return entries;
}

Status StdFileSystem::removeFile(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return Error{ErrorCode::InvalidArgument, "is a directory '" + pathForMessage(path) + "'"};
    fs::remove(path, ec);
    if (ec) return ioError("remove failed", path, ec);
    return Status::ok();
}

} // namespace dh
