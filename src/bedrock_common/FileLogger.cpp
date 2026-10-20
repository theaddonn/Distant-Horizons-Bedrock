#include "bedrock_common/FileLogger.h"

#include "ll/api/mod/NativeMod.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace dh::bedrock {

namespace {

constexpr long kMaxBytes = 8 * 1024 * 1024; // per file; dh.log + dh.log.1 + dh.log.2 <= 24 MB

std::mutex            gMutex;
FILE*                 gFile = nullptr;
long                  gSize = 0;
std::filesystem::path gPath;

char const* levelName(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    default:              return "ERROR";
    }
}

void rotateLocked();

void openLocked() {
    if (gFile) return;
    if (gPath.empty()) {
        auto mod = ll::mod::NativeMod::current();
        if (!mod) return;
        auto dir = mod->getModDir() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        gPath = dir / "dh.log";
    }
    if (_wfopen_s(&gFile, gPath.c_str(), L"ab") != 0) gFile = nullptr;
    if (gFile) {
        fseek(gFile, 0, SEEK_END);
        gSize = ftell(gFile);
        // First open in this process: start a fresh dh.log, the previous session becomes dh.log.1.
        static bool sessionStarted = false;
        if (!sessionStarted) {
            sessionStarted = true;
            if (gSize > 0) rotateLocked();
        }
    }
}

std::filesystem::path backupPath(int n) {
    auto p = gPath;
    p += "." + std::to_string(n);
    return p;
}

/// dh.log -> dh.log.1 -> dh.log.2 (the oldest is dropped). Never fails the caller: if the rename does not work
/// (file locked by a viewer) the current file is truncated instead, so the size stays bounded.
void rotateLocked() {
    if (gFile) fclose(gFile);
    gFile = nullptr;
    gSize = 0;
    std::error_code ec;
    std::filesystem::remove(backupPath(2), ec);
    std::filesystem::rename(backupPath(1), backupPath(2), ec);
    std::filesystem::rename(gPath, backupPath(1), ec);
    if (ec) {
        std::filesystem::remove(gPath, ec);
        if (_wfopen_s(&gFile, gPath.c_str(), L"wb") != 0) gFile = nullptr; // truncate in place as a last resort
        return;
    }
    openLocked();
}

} // namespace

void initLog() {
    std::lock_guard lock(gMutex);
    openLocked();
}

void logv(LogLevel level, char const* fmt, va_list args) {
    char    stackBuf[1024];
    va_list copy;
    va_copy(copy, args);
    int         n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    std::string big;
    char const* msg = stackBuf;
    if (n >= static_cast<int>(sizeof(stackBuf))) {
        big.resize(static_cast<size_t>(n) + 1);
        vsnprintf(big.data(), big.size(), fmt, copy);
        msg = big.c_str();
    } else if (n < 0) {
        msg = "<format error>";
    }
    va_end(copy);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[96];
    snprintf(
        line, sizeof(line), "[%02d:%02d:%02d.%03d][%s][%lx] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        levelName(level), static_cast<unsigned long>(GetCurrentThreadId())
    );

    {
        std::lock_guard lock(gMutex);
        openLocked();
        if (gFile && gSize > kMaxBytes) rotateLocked();
        if (gFile) {
            int a = fprintf(gFile, "%s%s\n", line, msg);
            if (a > 0) gSize += a;
            fflush(gFile);
        }
    }

    if (auto mod = ll::mod::NativeMod::current()) {
        auto& lg = mod->getLogger();
        switch (level) {
        case LogLevel::Debug: lg.debug("{}", msg); break;
        case LogLevel::Info:  lg.info("{}", msg); break;
        case LogLevel::Warn:  lg.warn("{}", msg); break;
        default:              lg.error("{}", msg); break;
        }
    }
}

void log(LogLevel level, char const* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(level, fmt, args);
    va_end(args);
}

} // namespace dh::bedrock
