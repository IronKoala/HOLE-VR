#include "core/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"
#include "core/perf.h"

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

// Everything one thread keeps between checks. MinGW builds `thread_local`
// as emulated TLS: every use of one is a call into __emutls_get_address
// (the 2026-11-06 profile of a test run put it at a fifth of each checked
// read). One Windows TLS slot holding a pointer to this, read straight from
// the thread's TEB, costs one instruction instead.
struct ThreadState {
    // Level 1: the last 4 regions this thread used.
    RegionCache cache[4];
    unsigned next = 0;
    // Level 2: every readable region this thread looked up during the
    // current game frame, sorted by start (2026-11-06). The 4-entry cache
    // alone kept missing when code hopped between many objects (each object,
    // its class and each name sit in different heap blocks), and every miss
    // was a VirtualQuery system call. Both levels are emptied when a new game
    // frame starts (NewFrame), so an answer is at most one frame old; the
    // copy itself is guarded (SafeCopy), so memory freed within the frame is
    // still caught.
    unsigned epoch = 0;
    std::vector<RegionCache> frame;
    // ScopedRegionCache: every region looked up while one is alive.
    int scope_depth = 0;
    std::vector<RegionCache>* scoped = nullptr;
    unsigned scoped_queries = 0;
    // Counts for the report (atomics on every read would cost more than they
    // measure), added up now and then.
    unsigned long long checks = 0, vq = 0, old_vq = 0;
    bool is_game = false;
    RegionCache old[4];  // the old cache, simulated for the count only
    unsigned old_next = 0;
};

constexpr size_t kMaxFrameRegions = 1024;
constexpr size_t kMaxScopedRegions = 4096;
// A function-local static: something may read memory before this file's
// globals are set up, and slot 0 would be someone else's.
DWORD TlsIndex() {
    static const DWORD index = TlsAlloc();
    return index;
}

ThreadState* NewThreadState() {
    // Kept for the thread's life: a few hundred bytes per thread that reads
    // game memory (the game, render and a few of our own threads).
    auto* ts = new ThreadState();
    ts->frame.reserve(256);
    TlsSetValue(TlsIndex(), ts);
    return ts;
}

inline ThreadState& TS() {
    ThreadState* ts;
    const DWORD g_tls = TlsIndex();
    if (g_tls < 64) {
        uintptr_t v;
        // TEB.TlsSlots[g_tls] (x64 TEB offset 0x1480).
        __asm__("movq %%gs:(%1), %0" : "=r"(v) : "r"(static_cast<uintptr_t>(0x1480 + g_tls * 8)));
        ts = reinterpret_cast<ThreadState*>(v);
    } else {
        ts = static_cast<ThreadState*>(TlsGetValue(g_tls));
    }
    return ts ? *ts : *NewThreadState();
}

std::atomic<unsigned> g_epoch{1};
std::atomic<unsigned long long> g_checks{0}, g_vq{0}, g_old_vq{0};
std::atomic<unsigned long long> g_game_checks{0}, g_game_vq{0}, g_game_old_vq{0};
std::atomic<long long> g_game_frames{0};

void FlushCounts(ThreadState& ts) {
    if (ts.is_game) {
        g_game_checks.fetch_add(ts.checks, std::memory_order_relaxed);
        g_game_vq.fetch_add(ts.vq, std::memory_order_relaxed);
        g_game_old_vq.fetch_add(ts.old_vq, std::memory_order_relaxed);
    }
    g_checks.fetch_add(ts.checks, std::memory_order_relaxed);
    g_vq.fetch_add(ts.vq, std::memory_order_relaxed);
    g_old_vq.fetch_add(ts.old_vq, std::memory_order_relaxed);
    ts.checks = ts.vq = ts.old_vq = 0;
}

const RegionCache* FindSorted(const std::vector<RegionCache>* v, uintptr_t p) {
    if (!v || v->empty()) return nullptr;
    auto it = std::upper_bound(v->begin(), v->end(), p,
                               [](uintptr_t x, const RegionCache& c) { return x < c.start; });
    if (it == v->begin()) return nullptr;
    --it;
    return p < it->end ? &*it : nullptr;
}

void InsertSorted(std::vector<RegionCache>* v, const RegionCache& c, size_t max) {
    if (!v || v->size() >= max) return;
    auto it = std::upper_bound(v->begin(), v->end(), c.start,
                               [](uintptr_t x, const RegionCache& r) { return x < r.start; });
    v->insert(it, c);
}

bool ProtectReadable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void CountOld(ThreadState& ts, uintptr_t p, const RegionCache& answer) {
    for (auto& c : ts.old)
        if (c.readable && p >= c.start && p < c.end) return;
    if (!FindSorted(ts.scoped, p)) ++ts.old_vq;
    ts.old[ts.old_next++ & 3] = answer;
}

bool RegionReadable(uintptr_t p, uintptr_t* region_end) {
    // Only "readable" answers are reused. A free or reserved region can be
    // huge and the game commits memory inside it all the time, so a cached
    // "unreadable" went stale and hid live objects; asking again costs one
    // VirtualQuery and bad pointers are rare.
    ThreadState& ts = TS();
    if (++ts.checks >= 8192 && !ts.is_game) FlushCounts(ts);
    const unsigned epoch = g_epoch.load(std::memory_order_relaxed);
    if (ts.epoch != epoch) {
        ts.epoch = epoch;
        for (auto& c : ts.cache) c = RegionCache{};
        ts.frame.clear();
    }
    for (auto& c : ts.cache) {
        if (c.readable && p >= c.start && p < c.end) {
            *region_end = c.end;
            CountOld(ts, p, c);
            return true;
        }
    }
    const RegionCache* known = FindSorted(ts.scoped, p);
    if (!known) known = FindSorted(&ts.frame, p);
    if (known) {
        RegionCache c = *known;
        ts.cache[ts.next++ & 3] = c;
        *region_end = c.end;
        CountOld(ts, p, c);
        return c.readable;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (ts.scope_depth > 0) ++ts.scoped_queries;
    ++ts.vq;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        *region_end = p + 1;
        CountOld(ts, p, RegionCache{});
        return false;
    }
    RegionCache& c = ts.cache[ts.next++ & 3];
    c.start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    c.end = c.start + mbi.RegionSize;
    c.readable = mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect);
    if (c.end > c.start) {
        if (ts.scope_depth > 0) InsertSorted(ts.scoped, c, kMaxScopedRegions);
        if (c.readable) InsertSorted(&ts.frame, c, kMaxFrameRegions);
    }
    CountOld(ts, p, c);
    *region_end = c.end;
    return c.readable;
}

std::string MemoryReport(double span_s) {
    const long long frames = g_game_frames.exchange(0);
    const unsigned long long gc = g_game_checks.exchange(0), gv = g_game_vq.exchange(0), go = g_game_old_vq.exchange(0);
    const unsigned long long ac = g_checks.exchange(0), av = g_vq.exchange(0), ao = g_old_vq.exchange(0);
    if (!ac || span_s <= 0) return {};
    char b[600];
    std::snprintf(b, sizeof(b),
                  "memory checks: game thread %.0f checked reads a frame, %.0f asked Windows (VirtualQuery) a frame; "
                  "the old 4-entry cache would have asked %.0f a frame. All threads: %.0f checks/s, %.0f asked "
                  "Windows/s (old cache %.0f/s)",
                  frames ? static_cast<double>(gc) / frames : 0.0, frames ? static_cast<double>(gv) / frames : 0.0,
                  frames ? static_cast<double>(go) / frames : 0.0, ac / span_s, av / span_s, ao / span_s);
    return b;
}
}  // namespace

void NewFrame() {
    static const bool registered = (perf::AddReportLine(&MemoryReport), true);
    (void)registered;
    ThreadState& ts = TS();
    ts.is_game = true;
    FlushCounts(ts);
    g_game_frames.fetch_add(1, std::memory_order_relaxed);
    g_epoch.fetch_add(1, std::memory_order_relaxed);
}

ScopedRegionCache::ScopedRegionCache() {
    ThreadState& ts = TS();
    if (ts.scope_depth++ == 0) {
        ts.scoped = new std::vector<RegionCache>();
        ts.scoped->reserve(256);
        ts.scoped_queries = 0;
    }
}

ScopedRegionCache::~ScopedRegionCache() {
    ThreadState& ts = TS();
    if (--ts.scope_depth == 0) {
        delete ts.scoped;
        ts.scoped = nullptr;
    }
}

unsigned ScopedRegionCache::Lookups() { return TS().scoped_queries; }
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
    ThreadState& ts = TS();
    for (auto& c : ts.cache)
        if (p >= c.start && p < c.end) c = RegionCache{};
    for (std::vector<RegionCache>* v : {ts.scoped, &ts.frame}) {
        if (!v) continue;
        for (auto it = v->begin(); it != v->end();) {
            if (p >= it->start && p < it->end) it = v->erase(it);
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
