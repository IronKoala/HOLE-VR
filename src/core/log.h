#pragma once
#include <string>

namespace holevr::log {
// Opens <mod folder>/hole-vr.log for this run. The logs of the three runs
// before it are kept as hole-vr.1.log (newest) .. hole-vr.3.log.
void Init(const std::wstring& mod_dir);
void Info(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Warn(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Error(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
// Lines are written by a background thread a few ms later (errors at once).
// Flush writes everything queued so far.
void Flush();
// Lines logged since the start (for the performance report).
long long LinesWritten();
}  // namespace holevr::log
