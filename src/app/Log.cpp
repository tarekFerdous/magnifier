#include "app/Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}  // namespace

void logInit(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) fclose(g_file);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
}

void logf(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, msg);

    OutputDebugStringA(line);
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}
