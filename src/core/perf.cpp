#include "core/perf.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"

namespace holevr::perf {
namespace {
std::mutex g_mutex;
// The default features currently register 51 distinct counters.
constexpr int kMaxCounters = 64;
Counter* g_counters[kMaxCounters];
int g_count = 0;
ReportFn g_lines[8];
int g_line_count = 0;
double g_last_report = 0.0;
double g_interval_s = 10.0;
}  // namespace

void Register(Counter* c) {
    if (!c) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < g_count; ++i)
        if (g_counters[i] == c) return;
    if (g_count < kMaxCounters) g_counters[g_count++] = c;
}

void AddReportLine(ReportFn fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < g_line_count; ++i)
        if (g_lines[i] == fn) return;
    if (g_line_count < 8) g_lines[g_line_count++] = fn;
}

void SetReportInterval(double seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_interval_s = seconds;
}

void MaybeReport() {
    double now = NowMs();
    std::string line;
    std::vector<std::string> chunks;
    ReportFn lines[8];
    int line_count = 0;
    double span_s;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_last_report == 0.0) {
            g_last_report = now;
            return;
        }
        span_s = (now - g_last_report) / 1000.0;
        if (span_s < g_interval_s) return;
        g_last_report = now;
        for (int i = 0; i < g_count; ++i) {
            Counter* c = g_counters[i];
            long long n = c->count.exchange(0);
            double total = c->total.exchange(0.0);
            double worst = c->worst.exchange(0.0);
            if (!n) continue;
            char buf[200];
            int len = std::snprintf(buf, sizeof(buf), " | %s %.2f ms avg, %.1f worst, %.0f/s", c->name, total / n,
                                    worst, n / span_s);
            if (c->per_second_total && len > 0 && len < static_cast<int>(sizeof(buf)))
                std::snprintf(buf + len, sizeof(buf) - len, " = %.1f ms/s", total / span_s);
            // log::Info has a 2048-byte message buffer. One large report
            // routinely lost its final counters (171 of 175 reports in the
            // 2026-11-03 run). Split only between complete counter records.
            constexpr size_t kMaxPayload = 1800;
            if (!line.empty() && line.size() + std::strlen(buf) > kMaxPayload) {
                chunks.push_back(std::move(line));
                line.clear();
            }
            line += buf;
        }
        line_count = g_line_count;
        for (int i = 0; i < line_count; ++i) lines[i] = g_lines[i];
    }
    // Everything below runs without our lock. The report lines take other
    // locks (the frame summary read the headset frame lock), and holding ours
    // while they waited froze the game on 2026-11-03: the render thread, inside
    // the frame lock, reported and asked for the frame lock again. Only the
    // thread that claimed this report (g_last_report above) gets here.
    if (!line.empty()) chunks.push_back(std::move(line));
    for (const std::string& chunk : chunks) log::Info("perf:%s", chunk.c_str());
    for (int i = 0; i < line_count; ++i) {
        std::string extra = lines[i](span_s);
        if (!extra.empty()) log::Info("%s", extra.c_str());
    }
}

}  // namespace holevr::perf
