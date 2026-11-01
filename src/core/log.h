#pragma once
#include <string>

namespace holevr::log {
// Opens <mod folder>/hole-vr.log, truncating the previous run's log.
void Init(const std::wstring& mod_dir);
void Info(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Warn(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void Error(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
}  // namespace holevr::log
