#pragma once
#include <string>

namespace holevr::config {
// Minimal key=value reader for <mod folder>/hole-vr.ini. Creates the file with
// defaults if it is missing, so players always have something to edit.
void Load(const std::wstring& mod_dir);
// The mod's folder (hole-vr beside the game exe), for files other than the ini.
const std::wstring& Dir();
bool GetBool(const char* key, bool fallback);
int GetInt(const char* key, int fallback);
float GetFloat(const char* key, float fallback);
}  // namespace holevr::config
