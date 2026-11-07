// Windows smoke test: samples only this test process, never the game.
// clang++ -std=c++20 -DNOMINMAX -Isrc tests/test_process_metrics.cpp src/core/process_metrics.cpp -lpsapi -o test_process_metrics.exe
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "core/process_metrics.h"

namespace { std::vector<std::string> lines; }
namespace holevr::log {
void Info(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    lines.emplace_back(message);
}
}

int main() {
    holevr::process_metrics::MaybeReport();
    holevr::process_metrics::MaybeReport();
    bool ok = lines.size() == 1;
    if (ok) for (const char* field : {"scope=game+mod", "private committed", "working set", "handles", "CPU baseline", "I/O baseline"})
        ok = ok && lines[0].find(field) != std::string::npos;
    if (!lines.empty()) std::puts(lines[0].c_str());
    std::puts(ok ? "all passed: sample complete, immediate repeat suppressed" : "FAILED");
    return ok ? 0 : 1;
}
