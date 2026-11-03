#include "core/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"

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
    // Only "readable" answers are reused. A free or reserved region can be
    // huge and the game commits memory inside it all the time, so a cached
    // "unreadable" went stale and hid live objects; asking again costs one
    // VirtualQuery and bad pointers are rare.
    for (auto& c : t_cache) {
        if (c.readable && p >= c.start && p < c.end) {
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
// ------------------------------------------------------------ SafeCopy --
#ifdef _WIN32
// holevr_guarded_copy(dst, src, len) copies with plain moves and returns 1.
// It is a leaf function with no stack use, so when one of its moves faults
// the vectored handler below can simply point RIP at holevr_guarded_copy_fail,
// which returns 0. Sizes 1/2/4/8 are copied with a single move so a write of
// a float or pointer the game is reading is never seen half done.
extern "C" int holevr_guarded_copy(void* dst, const void* src, size_t len);
extern "C" char holevr_guarded_copy_fail[];
extern "C" char holevr_guarded_copy_end[];

asm(R"(
    .text
    .p2align 4
    .globl holevr_guarded_copy
    .globl holevr_guarded_copy_fail
    .globl holevr_guarded_copy_end
holevr_guarded_copy:
    cmp $8, %r8
    je 8f
    cmp $4, %r8
    je 4f
    cmp $2, %r8
    je 2f
    cmp $1, %r8
    je 1f
    jmp 10f
8:  movq (%rdx), %rax
    movq %rax, (%rcx)
    jmp 9f
4:  movl (%rdx), %eax
    movl %eax, (%rcx)
    jmp 9f
2:  movw (%rdx), %ax
    movw %ax, (%rcx)
    jmp 9f
1:  movb (%rdx), %al
    movb %al, (%rcx)
    jmp 9f
10: cmp $8, %r8
    jb 12f
11: movq (%rdx), %rax
    movq %rax, (%rcx)
    add $8, %rdx
    add $8, %rcx
    sub $8, %r8
    cmp $8, %r8
    jae 11b
12: test %r8, %r8
    jz 9f
13: movb (%rdx), %al
    movb %al, (%rcx)
    inc %rdx
    inc %rcx
    dec %r8
    jnz 13b
9:  mov $1, %eax
    ret
holevr_guarded_copy_fail:
    xor %eax, %eax
    ret
holevr_guarded_copy_end:
    nop
)");

namespace {
std::atomic<unsigned> g_caught{0};
thread_local uintptr_t t_fault_addr = 0;

LONG CALLBACK GuardedCopyHandler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = info->ContextRecord->Rip;
    if (rip < reinterpret_cast<uintptr_t>(&holevr_guarded_copy) ||
        rip >= reinterpret_cast<uintptr_t>(holevr_guarded_copy_fail))
        return EXCEPTION_CONTINUE_SEARCH;  // not ours: the game's own crash handling runs as before
    t_fault_addr = info->ExceptionRecord->NumberParameters >= 2
                       ? static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                       : 0;
    info->ContextRecord->Rip = reinterpret_cast<uintptr_t>(holevr_guarded_copy_fail);
    g_caught.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool InstallGuardHandler() {
    // First in line, so it sees the fault before the game's crash reporter.
    // The game's own vectored handlers (D3D12 RHI) only look at code 0x87A.
    bool ok = AddVectoredExceptionHandler(1, &GuardedCopyHandler) != nullptr;
    log::Info("memory: fault guard %s (a read of game memory freed under us is caught, not a crash)",
              ok ? "on" : "FAILED to install");
    return ok;
}

// The faulting address is no longer readable: drop whatever this thread's
// cache remembered about it so the next IsReadable asks Windows again.
void ForgetRegion(uintptr_t p) {
    for (auto& c : t_cache)
        if (p >= c.start && p < c.end) c = RegionCache{};
    if (t_scoped) {
        for (auto it = t_scoped->begin(); it != t_scoped->end();) {
            if (p >= it->start && p < it->end) it = t_scoped->erase(it);
            else ++it;
        }
    }
}
}  // namespace

bool SafeCopy(void* dst, const void* src, size_t len) {
    static const bool installed = InstallGuardHandler();
    if (!installed) {  // can't happen on Windows 10/11; behave as before
        std::memcpy(dst, src, len);
        return true;
    }
    if (len == 0) return true;
    if (holevr_guarded_copy(dst, src, len)) return true;
    const uintptr_t at = t_fault_addr;
    ForgetRegion(at);
    ForgetRegion(reinterpret_cast<uintptr_t>(src));
    ForgetRegion(reinterpret_cast<uintptr_t>(dst));
    const unsigned n = g_caught.load(std::memory_order_relaxed);
    if (n <= 5 || n % 100 == 0)
        log::Warn("memory: caught a fault at %p (game memory freed under us; %u caught so far), carried on",
                  reinterpret_cast<void*>(at), n);
    return false;
}

unsigned CaughtFaults() { return g_caught.load(std::memory_order_relaxed); }
#else
// Native test builds (tests/win_shim): no exception handling to hook into.
bool SafeCopy(void* dst, const void* src, size_t len) {
    std::memcpy(dst, src, len);
    return true;
}
unsigned CaughtFaults() { return 0; }
#endif

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
