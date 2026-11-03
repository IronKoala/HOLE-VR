#pragma once
#include <string>
#include <utility>
#include <vector>

namespace holevr::config {
// Minimal key=value reader for <mod folder>/hole-vr.ini. Creates the file with
// defaults if it is missing, so players always have something to edit.
void Load(const std::wstring& mod_dir);
// The mod's folder (hole-vr beside the game exe), for files other than the ini.
const std::wstring& Dir();
bool GetBool(const char* key, bool fallback);
int GetInt(const char* key, int fallback);
float GetFloat(const char* key, float fallback);
std::string GetString(const char* key, const char* fallback);
// Every key starting with prefix, with the prefix removed, e.g. "cvar." lines.
std::vector<std::pair<std::string, std::string>> WithPrefix(const char* prefix);

// Changes a setting while the game runs (the VR tab in Options): the value
// is used from now on and written into hole-vr.ini in place, keeping the
// file's comments and order. Returns false if the file could not be written
// (the new value is still used for this run). Game thread.
bool Set(const char* key, const std::string& value);
// Settings that modules copy into their own structs at start-up are read
// again by these, called after Set() changes something. Up to 16.
using ChangeFn = void (*)();
void AddChangeListener(ChangeFn fn);
// Calls every change listener (after one or more Set calls). Game thread.
void NotifyChanged();
}  // namespace holevr::config
