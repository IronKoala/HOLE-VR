#pragma once
#include <string>
#include <utility>
#include <vector>

namespace holevr::config {
// Minimal key=value reader for <mod folder>/hole-vr.ini. Creates the file with
// defaults if it is missing, so players always have something to edit.
void Load(const std::wstring& mod_dir);
bool GetBool(const char* key, bool fallback);
int GetInt(const char* key, int fallback);
float GetFloat(const char* key, float fallback);
std::string GetString(const char* key, const char* fallback);
// Every key starting with prefix, with the prefix removed, e.g. "cvar." lines.
std::vector<std::pair<std::string, std::string>> WithPrefix(const char* prefix);
}  // namespace holevr::config
