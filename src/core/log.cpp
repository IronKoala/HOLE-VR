#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace holevr::log {
namespace {
constexpr int kKeepOldLogs = 3;
// g_mutex guards g_pending; g_file_mutex guards the file itself.
std::mutex g_mutex;
std::mutex g_file_mutex;
FILE* g_file = nullptr;
std::vector<std::string> g_pending;
std::atomic<HANDLE> g_wake{nullptr};
std::atomic<long long> g_lines{0};

// Writes everything queued so far. Called by the writer thread, and directly
// for errors so an error line is on disk before the call returns.
void Drain() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        lines.swap(g_pending);
    }
    if (lines.empty()) return;
    std::lock_guard<std::mutex> lock(g_file_mutex);
    if (!g_file) return;
    for (const std::string& l : lines) fputs(l.c_str(), g_file);
    fflush(g_file);
}

// Disk writes used to happen on the calling thread, a write and a flush per
// line, and OutputDebugString raised an exception per line: per-frame log
// lines from the game thread cost real frame time (2026-11-03, the rack trace
// logged every frame). Lines are now handed to this thread, which writes
// them within a few milliseconds and flushes once per batch, so a crash still
// leaves the lines that led up to it.
void WriterLoop(HANDLE wake) {
    for (;;) {
        WaitForSingleObject(wake, 100);
        Drain();
    }
}

void Write(const char* level, const char* fmt, va_list args, bool now) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, level, msg);
    if (IsDebuggerPresent()) OutputDebugStringA(line);
    g_lines.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.emplace_back(line);
    }
    HANDLE wake = g_wake.load();
    if (now || !wake) Drain();
    else SetEvent(wake);
}
}  // namespace

void Init(const std::wstring& mod_dir) {
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (g_file) return;
        // Keep the logs of the last few runs: starting the game again right after
        // a playtest must not wipe the playtest's log.
        //   hole-vr.log (this run), hole-vr.1.log (the run before), .2, .3
        auto name = [&](int n) {
            return n == 0 ? mod_dir + L"\\hole-vr.log"
                          : mod_dir + L"\\hole-vr." + std::to_wstring(n) + L".log";
        };
        for (int n = kKeepOldLogs; n >= 1; --n)
            MoveFileExW(name(n - 1).c_str(), name(n).c_str(), MOVEFILE_REPLACE_EXISTING);
        g_file = _wfopen(name(0).c_str(), L"w");
        if (g_file) {
            // Lines carry only the time of day; this dates the run.
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(g_file, "HOLE VR log, run started %04d-%02d-%02d %02d:%02d:%02d local time\n", t.wYear,
                    t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            fflush(g_file);
        }
    }
    // Init runs inside DllMain: CreateThread is safe there (the thread starts
    // once the loader lock is released); no waiting on it here.
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return;
    if (HANDLE t = CreateThread(
            nullptr, 0, [](LPVOID p) -> DWORD { WriterLoop(static_cast<HANDLE>(p)); return 0; }, wake, 0, nullptr)) {
        CloseHandle(t);
        g_wake = wake;
    } else {
        CloseHandle(wake);
    }
}

void Flush() { Drain(); }

long long LinesWritten() { return g_lines.load(std::memory_order_relaxed); }

#define HOLEVR_LOG_FN(name, level, now)  \
    void name(const char* fmt, ...) {    \
        va_list args;                    \
        va_start(args, fmt);             \
        Write(level, fmt, args, now);    \
        va_end(args);                    \
    }
HOLEVR_LOG_FN(Info, "info", false)
HOLEVR_LOG_FN(Warn, "warn", false)
HOLEVR_LOG_FN(Error, "error", true)
#undef HOLEVR_LOG_FN

}  // namespace holevr::log
