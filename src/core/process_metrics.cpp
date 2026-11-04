#include "core/process_metrics.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/perf.h"

namespace holevr::process_metrics {
namespace {
constexpr double kIntervalMs = 30000.0;
constexpr double kMiB = 1024.0 * 1024.0;

struct State {
    bool sampled = false;
    double last_sample_ms = 0.0;
    bool have_memory = false, have_cpu = false, have_io = false;
    SIZE_T first_private = 0, last_private = 0;
    uint64_t cpu_ticks = 0;
    double cpu_at_ms = 0.0, io_at_ms = 0.0;
    IO_COUNTERS io{};
} S;

uint64_t Ticks(const FILETIME& t) {
    return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
}  // namespace

void MaybeReport() {
    const double now = perf::NowMs();
    if (S.sampled && now - S.last_sample_ms < kIntervalMs) return;
    S.sampled = true;
    S.last_sample_ms = now;

    const HANDLE process = GetCurrentProcess();
    std::string line = "process: scope=game+mod";
    char part[512];
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        if (!S.have_memory) S.first_private = memory.PrivateUsage;
        std::snprintf(part, sizeof(part),
                      " | private committed %.1f MiB (change %+.1f since previous, %+.1f since baseline)"
                      " | working set %.1f MiB (peak %.1f)",
                      memory.PrivateUsage / kMiB,
                      S.have_memory ? (static_cast<double>(memory.PrivateUsage) - S.last_private) / kMiB : 0.0,
                      (static_cast<double>(memory.PrivateUsage) - S.first_private) / kMiB,
                      memory.WorkingSetSize / kMiB, memory.PeakWorkingSetSize / kMiB);
        line += part;
        S.last_private = memory.PrivateUsage;
        S.have_memory = true;
    } else {
        std::snprintf(part, sizeof(part), " | memory unavailable (Windows error %lu)", GetLastError());
        line += part;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(process, &handles)) {
        std::snprintf(part, sizeof(part), " | handles %lu", handles);
        line += part;
    } else {
        std::snprintf(part, sizeof(part), " | handles unavailable (Windows error %lu)", GetLastError());
        line += part;
    }

    FILETIME created{}, ended{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &ended, &kernel, &user)) {
        const uint64_t cpu = Ticks(kernel) + Ticks(user);
        if (S.have_cpu && now > S.cpu_at_ms && cpu >= S.cpu_ticks) {
            // All process threads combined, so this may exceed 1000 ms/s.
            const double cpu_ms_per_s = (cpu - S.cpu_ticks) / 10000.0 / ((now - S.cpu_at_ms) / 1000.0);
            std::snprintf(part, sizeof(part), " | CPU %.1f ms/s across all threads", cpu_ms_per_s);
            line += part;
        } else {
            line += " | CPU baseline";
        }
        S.cpu_ticks = cpu;
        S.cpu_at_ms = now;
        S.have_cpu = true;
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(process, &io)) {
        if (S.have_io && now > S.io_at_ms && io.ReadTransferCount >= S.io.ReadTransferCount &&
            io.WriteTransferCount >= S.io.WriteTransferCount) {
            const double seconds = (now - S.io_at_ms) / 1000.0;
            std::snprintf(part, sizeof(part), " | I/O read %.2f MiB/s, write %.2f MiB/s",
                          (io.ReadTransferCount - S.io.ReadTransferCount) / kMiB / seconds,
                          (io.WriteTransferCount - S.io.WriteTransferCount) / kMiB / seconds);
            line += part;
        } else {
            line += " | I/O baseline";
        }
        S.io = io;
        S.io_at_ms = now;
        S.have_io = true;
    }
    log::Info("%s", line.c_str());
}

}  // namespace holevr::process_metrics
