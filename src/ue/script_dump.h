#pragma once
#include <string>
#include <vector>

#include "ue/objects.h"

// Readable listings of Blueprint bytecode (Kismet VM script), so we can see
// what a game function really does (which functions it calls, in what order,
// with which values) without the game's editor or unpacking its paks.
//
// Pointers in the in-memory bytecode are resolved through the live object
// array and the property chain; names through the FName pool. An opcode this
// decoder does not know stops that function's listing and prints the rest of
// its bytes as hex, so a listing is never silently wrong.
namespace holevr::ue::script {

// One function: "  0x0012: statement" lines. Empty if it has no bytecode.
std::string Disassemble(UObject function);

// Writes dump/script/<Class>.txt for every loaded Blueprint class whose path
// contains one of `filters` (all functions, its properties' names resolved).
// Returns how many classes were written.
int DumpClasses(const std::wstring& dump_dir, const std::vector<std::string>& filters);

}  // namespace holevr::ue::script
