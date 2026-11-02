#pragma once
#include <cstddef>

namespace holevr::hooks {

// Overwrite one slot of a vtable that lives in a module's read-only data.
// Affects every object of that class. Returns the previous function.
void* PatchVtableSlot(void** vtable, size_t index, void* replacement);

// Redirect a function the game exe imports, by rewriting its IAT slot.
// Returns the original function, or nullptr if the import was not found.
void* PatchExeImport(const char* dll_name, const char* function_name, void* replacement);

}  // namespace holevr::hooks
