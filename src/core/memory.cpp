#include "core/memory.h"

#include <windows.h>

#include <algorithm>
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

// ScopedRegionCache: every region looked up while one is alive, sorted by
// start address. Regions VirtualQuery reports never overlap.
constexpr size_t kMaxScopedRegions = 4096;
thread_local int t_scope_depth = 0;
thread_local std::vector<RegionCache>* t_scoped = nullptr;
thread_local unsigned t_scoped_queries = 0;

const RegionCache* FindScoped(uintptr_t p) {
    if (!t_scoped || t_scoped->empty()) return nullptr;
    auto it = std::upper_bound(t_scoped->begin(), t_scoped->end(), p,
                               [](uintptr_t v, const RegionCache& c) { return v < c.start; });
    if (it == t_scoped->begin()) return nullptr;
    --it;
    return p < it->end ? &*it : nullptr;
}

void RememberScoped(const RegionCache& c) {
    if (!t_scoped || t_scoped->size() >= kMaxScopedRegions) return;
    auto it = std::upper_bound(t_scoped->begin(), t_scoped->end(), c.start,
                               [](uintptr_t v, const RegionCache& r) { return v < r.start; });
    t_scoped->insert(it, c);
}

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
    if (const RegionCache* s = FindScoped(p)) {
        t_cache[t_next++ & 3] = *s;
        *region_end = s->end;
        return s->readable;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (t_scope_depth > 0) ++t_scoped_queries;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        *region_end = p + 1;
        return false;
    }
    RegionCache& c = t_cache[t_next++ & 3];
    c.start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    c.end = c.start + mbi.RegionSize;
    c.readable = mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect);
    if (t_scope_depth > 0 && c.end > c.start) RememberScoped(c);
    *region_end = c.end;
    return c.readable;
}
}  // namespace

ScopedRegionCache::ScopedRegionCache() {
    if (t_scope_depth++ == 0) {
        t_scoped = new std::vector<RegionCache>();
        t_scoped->reserve(256);
        t_scoped_queries = 0;
    }
}

ScopedRegionCache::~ScopedRegionCache() {
    if (--t_scope_depth == 0) {
        delete t_scoped;
        t_scoped = nullptr;
    }
}

unsigned ScopedRegionCache::Lookups() { return t_scoped_queries; }

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

bool RegionMemo::IsReadable(uintptr_t p, size_t len) {
    if (p < 0x10000 || p >= 0x00007FFFFFFFFFFFull) return false;
    uintptr_t end = p + len;
    while (p < end) {
        auto it = std::upper_bound(regions_.begin(), regions_.end(), p,
                                   [](uintptr_t v, const Region& r) { return v < r.start; });
        if (it != regions_.begin() && p < (it - 1)->end) {
            if (!(it - 1)->readable) return false;
            p = (it - 1)->end;
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
        Region r;
        r.start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.end = r.start + mbi.RegionSize;
        r.readable = mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect);
        if (r.end <= p) return false;  // never expected; don't loop on it
        if (regions_.size() >= 8192) regions_.clear();
        regions_.insert(std::lower_bound(regions_.begin(), regions_.end(), r.start,
                                         [](const Region& a, uintptr_t v) { return a.start < v; }),
                        r);
        if (!r.readable) return false;
        p = r.end;
    }
    return true;
}

bool IsWritable(uintptr_t p, size_t len) {
    if (p < 0x10000 || p >= 0x00007FFFFFFFFFFFull) return false;
    uintptr_t end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
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
