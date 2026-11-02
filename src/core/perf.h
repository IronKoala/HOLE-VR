#pragma once
// Lightweight timing counters for diagnosing frame-rate problems. Each
// counter accumulates total and worst time between log reports.
#include <windows.h>

#include <atomic>

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
    void Add(double ms) {
        total.store(total.load() + ms);
        if (ms > worst.load()) worst.store(ms);
        count.fetch_add(1);
    }
};

// Registers a counter (static storage) to be included in the periodic report.
void Register(Counter* c);
// Logs every registered counter (average and worst per call, calls per second)
// at most once per 10 s, then resets them. Call from any frequent place.
void MaybeReport();

struct Scope {
    Counter& c;
    double start;
    explicit Scope(Counter& counter) : c(counter), start(NowMs()) {}
    ~Scope() { c.Add(NowMs() - start); }
};

}  // namespace holevr::perf
