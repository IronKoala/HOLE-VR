#pragma once
// Lightweight timing counters for diagnosing frame-rate problems. Each
// counter accumulates total and worst time between log reports.
#include <windows.h>

#include <atomic>
#include <string>

namespace holevr::perf {

inline double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / static_cast<double>(freq.QuadPart);
}

struct Counter {
    const char* name;
    std::atomic<double> total{0.0};
    std::atomic<double> worst{0.0};
    std::atomic<long long> count{0};
    // Also print the total time per second ("= 12.3 ms/s"): for counters hit
    // thousands of times a frame, where the average alone hides the cost.
    bool per_second_total = false;
    void Add(double ms) {
        // Some counters receive calls from several engine threads. A
        // load/store pair loses simultaneous samples (or restores a total
        // that the reporter just cleared). Keep each update atomic.
        total.fetch_add(ms, std::memory_order_relaxed);
        double observed = worst.load(std::memory_order_relaxed);
        while (ms > observed &&
               !worst.compare_exchange_weak(observed, ms, std::memory_order_relaxed)) {}
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

// Registers a counter (static storage) to be included in the periodic report.
void Register(Counter* c);
// Logs every registered counter (average and worst per call, calls per second)
// at most once per 10 s, then resets them. Call from any frequent place, but
// never while holding a lock that a report line takes (the report lines run
// on the calling thread).
void MaybeReport();
// Tests only: report more often than every 10 s.
void SetReportInterval(double seconds);

// Extra lines for the same 10 s report (e.g. the frame summary). Called with
// the report's span in seconds, on whichever thread reports (game or render
// thread), without perf's lock; return an empty string for nothing. Keep them
// lock-free or on leaf locks: a report line that waited on the render
// thread's frame lock froze the game (2026-11-03).
using ReportFn = std::string (*)(double span_s);
void AddReportLine(ReportFn fn);

struct Scope {
    Counter& c;
    double start;
    explicit Scope(Counter& counter) : c(counter), start(NowMs()) {}
    ~Scope() { c.Add(NowMs() - start); }
};

}  // namespace holevr::perf
