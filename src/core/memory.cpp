#include "core/memory.h"

#include <windows.h>

#include <cstdlib>

namespace holevr::mem {

const Section* Module::Find(const char* name) const {
    for (const auto& s : sections)
        if (s.name == name) return &s;
    return nullptr;
}

const Module& Game() {
    static Module m = [] {
        Module mod;
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        mod.base = base;
        mod.size = nt->OptionalHeader.SizeOfImage;
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            char name[9] = {};
            std::memcpy(name, sec->Name, 8);
            mod.sections.push_back(
                {name, base + sec->VirtualAddress,
                 static_cast<size_t>(sec->Misc.VirtualSize ? sec->Misc.VirtualSize
                                                           : sec->SizeOfRawData)});
        }
        return mod;
    }();
    return m;
}

namespace {
struct RegionCache {
    uintptr_t start = 0;
    uintptr_t end = 0;
    bool readable = false;
};
thread_local RegionCache t_cache[4];
thread_local unsigned t_next = 0;

bool ProtectReadable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool RegionReadable(uintptr_t p, uintptr_t* region_end) {
    for (auto& c : t_cache) {
        if (p >= c.start && p < c.end) {
            *region_end = c.end;
            return c.readable;
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        *region_end = p + 1;
        return false;
    }
    RegionCache& c = t_cache[t_next++ & 3];
    c.start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    c.end = c.start + mbi.RegionSize;
    c.readable = mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect);
    *region_end = c.end;
    return c.readable;
}
}  // namespace

bool IsReadable(uintptr_t p, size_t len) {
    if (p < 0x10000 || p >= 0x00007FFFFFFFFFFFull) return false;
    uintptr_t end = p + len;
    while (p < end) {
        uintptr_t region_end;
        if (!RegionReadable(p, &region_end)) return false;
        p = region_end;
    }
    return true;
}

uintptr_t FindBytes(uintptr_t start, size_t size, const void* bytes, size_t len) {
    if (len == 0 || size < len) return 0;
    auto* hay = reinterpret_cast<const unsigned char*>(start);
    auto* needle = static_cast<const unsigned char*>(bytes);
    for (size_t i = 0; i + len <= size; ++i) {
        if (hay[i] == needle[0] && std::memcmp(hay + i, needle, len) == 0)
            return start + i;
    }
    return 0;
}

uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            bytes.push_back(-1);
            while (*p == '?') ++p;
            continue;
        }
        bytes.push_back(static_cast<int>(std::strtoul(p, const_cast<char**>(&p), 16)));
    }
    auto* data = reinterpret_cast<const unsigned char*>(start);
    for (size_t i = 0; i + bytes.size() <= size; ++i) {
        bool ok = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] != -1 && data[i + j] != bytes[j]) { ok = false; break; }
        }
        if (ok) return start + i;
    }
    return 0;
}

}  // namespace holevr::mem
