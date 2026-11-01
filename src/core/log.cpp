#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace holevr::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;

void Write(const char* level, const char* fmt, va_list args) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, level, msg);
    std::lock_guard<std::mutex> lock(g_mutex);
    OutputDebugStringA(line);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);  // Flush every line so a crash never eats the cause.
    }
}
}  // namespace

void Init(const std::wstring& mod_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::wstring path = mod_dir + L"\\hole-vr.log";
    g_file = _wfopen(path.c_str(), L"w");
}

#define HOLEVR_LOG_FN(name, level)       \
    void name(const char* fmt, ...) {    \
        va_list args;                    \
        va_start(args, fmt);             \
        Write(level, fmt, args);         \
        va_end(args);                    \
    }
HOLEVR_LOG_FN(Info, "info")
HOLEVR_LOG_FN(Warn, "warn")
HOLEVR_LOG_FN(Error, "error")
#undef HOLEVR_LOG_FN

}  // namespace holevr::log
