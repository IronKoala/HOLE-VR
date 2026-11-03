#include "core/perf.h"

#include <mutex>
#include <string>

#include "core/log.h"

namespace holevr::perf {
namespace {
std::mutex g_mutex;
Counter* g_counters[48];
int g_count = 0;
ReportFn g_lines[8];
int g_line_count = 0;
double g_last_report = 0.0;
}  // namespace

void Register(Counter* c) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_count < 48) g_counters[g_count++] = c;
}

void AddReportLine(ReportFn fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < g_line_count; ++i)
        if (g_lines[i] == fn) return;
    if (g_line_count < 8) g_lines[g_line_count++] = fn;
}

void MaybeReport() {
    double now = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_last_report == 0.0) {
        g_last_report = now;
        return;
    }
    double span_s = (now - g_last_report) / 1000.0;
    if (span_s < 10.0) return;
    g_last_report = now;
    std::string line;
    for (int i = 0; i < g_count; ++i) {
        Counter* c = g_counters[i];
        long long n = c->count.exchange(0);
        double total = c->total.exchange(0.0);
        double worst = c->worst.exchange(0.0);
        if (!n) continue;
        char buf[200];
        int len = std::snprintf(buf, sizeof(buf), " | %s %.2f ms avg, %.1f worst, %.0f/s", c->name, total / n, worst,
                                n / span_s);
        if (c->per_second_total && len > 0 && len < static_cast<int>(sizeof(buf)))
            std::snprintf(buf + len, sizeof(buf) - len, " = %.1f ms/s", total / span_s);
        line += buf;
    }
    log::Info("perf:%s", line.c_str());
    for (int i = 0; i < g_line_count; ++i) {
        std::string extra = g_lines[i](span_s);
        if (!extra.empty()) log::Info("%s", extra.c_str());
    }
}

}  // namespace holevr::perf
