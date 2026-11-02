#pragma once
#include <string>

namespace holevr::log {
// Opens <mod folder>/hole-vr.log for this run. The logs of the three runs
// before it are kept as hole-vr.1.log (newest) .. hole-vr.3.log.
void Init(const std::wstring& mod_dir);
void Info(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Warn(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Error(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
}  // namespace holevr::log
