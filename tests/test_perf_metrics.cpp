// Windows regression for concurrent counters, duplicate registration and
// complete reports through the logger's 2048-byte message limit.
// x86_64-w64-mingw32-g++ -std=c++20 -O2 -Isrc tests/test_perf_metrics.cpp src/core/perf.cpp -static -o test_perf_metrics.exe && wine test_perf_metrics.exe
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/perf.h"

namespace {
std::mutex log_mutex;
std::vector<std::string> logged;
int failures = 0;
void Expect(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
}
namespace holevr::log {
void Info(const char* format, ...) {
    // Match the production logger's limit; an oversized input gets cut.
    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(log_mutex);
    logged.emplace_back(message);
}
}

int main() {
    using holevr::perf::Counter;
    Counter shared{"shared"};
    constexpr int threads = 8, samples = 20000;
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < samples; ++i) shared.Add(t + 1.0);
    });
    start = true;
    for (std::thread& worker : workers) worker.join();
    Expect(shared.count.load() == threads * samples, "all concurrent samples counted");
    Expect(shared.total.load() == samples * (threads * (threads + 1) / 2), "no concurrent totals lost");
    Expect(shared.worst.load() == threads, "largest concurrent sample retained");

    static Counter counters[60];
    static std::string names[60];
    for (int i = 0; i < 60; ++i) {
        names[i] = "test-counter-" + std::to_string(i) + "-complete-record";
        counters[i].name = names[i].c_str();
        holevr::perf::Register(&counters[i]);
        holevr::perf::Register(&counters[i]); // must not consume another slot
    }
    holevr::perf::MaybeReport();
    holevr::perf::SetReportInterval(0.0);
    for (Counter& counter : counters) counter.Add(1.0);
    holevr::perf::MaybeReport();
    Expect(logged.size() > 1, "long report split into several messages");
    std::string report;
    for (const std::string& line : logged) {
        Expect(line.size() < 2047, "message fits logger without truncation");
        report += line;
    }
    for (const std::string& name : names) {
        const size_t pos = report.find(name);
        Expect(pos != std::string::npos, "all counters including those after 48 retained");
        if (pos != std::string::npos)
            Expect(report.find(name, pos + name.size()) == std::string::npos, "counter emitted once");
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
