// Windows test for perf::MaybeReport's locking. Cross-compile and run (wine works too):
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -Isrc tests/test_perf_report.cpp src/core/perf.cpp
//       src/core/log.cpp -static -o test_perf_report.exe && wine test_perf_report.exe
//
// 2026-11-03 the game froze 5-20 s into every level: the render thread, inside
// the headset frame lock, made the 10 s perf report, and the report's frame
// timing line asked for the frame lock again. perf's own lock was held all
// the while, so the game thread (which reports too) stopped at its next
// report and the whole game hung, with no crash report. Report lines now run
// without perf's lock, and the render thread reports after leaving the frame
// lock. Each case runs on a thread with a time limit: a hang fails the test.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/perf.h"

using namespace holevr;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static std::mutex g_frame_mutex;  // stands in for the headset frame lock
static std::atomic<int> g_line_calls{0};
static std::atomic<bool> g_line_waiting{false};
static perf::Counter g_counter{"test counter"};

// Like the old frame timing line: takes the frame lock.
static std::string FrameLine(double) {
    g_line_waiting = true;
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_line_waiting = false;
    ++g_line_calls;
    return "frame timing: test line";
}

// A line that registers more (takes perf's own lock).
static std::string NestedLine(double) {
    static perf::Counter later{"registered from a report line"};
    perf::Register(&later);
    return {};
}

static bool RunWithin(LPTHREAD_START_ROUTINE fn, DWORD ms) {
    HANDLE t = CreateThread(nullptr, 0, fn, nullptr, 0, nullptr);
    bool done = WaitForSingleObject(t, ms) == WAIT_OBJECT_0;
    CloseHandle(t);
    return done;
}

static void MakeReportDue() {
    perf::SetReportInterval(0.05);
    Sleep(80);
}

// 1. Game thread reports while the render thread holds the frame lock and
// reports too (the two orders that used to deadlock against each other).
static DWORD WINAPI GameThreadReport(LPVOID) {
    perf::MaybeReport();
    return 0;
}
static DWORD WINAPI CrossedLocks(LPVOID) {
    MakeReportDue();
    HANDLE game;
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);  // render thread in RunFrame
        game = CreateThread(nullptr, 0, GameThreadReport, nullptr, 0, nullptr);
        // Wait until the game thread's report line is waiting on our lock.
        for (int i = 0; i < 200 && !g_line_waiting; ++i) Sleep(5);
        perf::SetReportInterval(1000.0);  // our own report isn't due
        perf::MaybeReport();              // used to wait forever for perf's lock
    }
    bool ok = WaitForSingleObject(game, 3000) == WAIT_OBJECT_0;
    CloseHandle(game);
    return ok ? 0 : 1;
}

// 2. The render thread's order after the fix: frame work under the lock,
// the report after it.
static DWORD WINAPI RenderThreadReport(LPVOID) {
    MakeReportDue();
    {
        std::lock_guard<std::mutex> lock(g_frame_mutex);
        g_counter.Add(1.0);
    }
    perf::MaybeReport();
    return 0;
}

// 3. A report line may use perf itself.
static DWORD WINAPI ReportLineUsesPerf(LPVOID) {
    MakeReportDue();
    perf::MaybeReport();
    return 0;
}

int main() {
    perf::Register(&g_counter);
    perf::AddReportLine(&FrameLine);
    perf::MaybeReport();  // starts the first window

    g_counter.Add(1.0);
    int before = g_line_calls;
    EXPECT(RunWithin(CrossedLocks, 5000));
    EXPECT(g_line_calls == before + 1);

    before = g_line_calls;
    EXPECT(RunWithin(RenderThreadReport, 5000));
    EXPECT(g_line_calls == before + 1);

    perf::AddReportLine(&NestedLine);
    EXPECT(RunWithin(ReportLineUsesPerf, 5000));

    // Not due: no report, no line.
    perf::SetReportInterval(1000.0);
    before = g_line_calls;
    perf::MaybeReport();
    EXPECT(g_line_calls == before);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    std::fflush(stdout);
    // A hung case leaves its thread blocked; leave without waiting for it.
    ExitProcess(failures ? 1 : 0);
}
