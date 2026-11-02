#pragma once
#include <string>

namespace holevr::ue::dumper {
// Writes the game's reflection data to <mod folder>\dump\:
//   objects.txt       every live object, "index  Class /Path.Name"
//   classes.txt       every class and struct with properties and functions
//   game_classes.txt  same, limited to the game's own content (/Game/, /Script/HOLE...)
void DumpAll(const std::wstring& mod_dir);
}  // namespace holevr::ue::dumper
