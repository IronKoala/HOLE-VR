#include "hooks/hooks.h"

#include <windows.h>

#include <cstring>

#include "core/memory.h"

namespace holevr::hooks {

void* PatchVtableSlot(void** vtable, size_t index, void* replacement) {
    void** slot = vtable + index;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* original = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return original;
}

void* PatchExeImport(const char* dll_name, const char* function_name, void* replacement) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll_name) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function_name) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old);
            void* original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), old, &old);
            return original;
        }
    }
    return nullptr;
}

}  // namespace holevr::hooks
