#include "core/sampler.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cxxabi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace holevr::sampler {
namespace {

std::atomic<DWORD> g_game_tid{0}, g_present_tid{0};
std::atomic<long> g_game_frames{0}, g_present_frames{0};
std::atomic<bool> g_started{false};

// ------------------------------------------------------------- modules --
// Only the sampler thread touches these.
struct Export {
    uint32_t rva;
    std::string name;
};
struct Module {
    uintptr_t base = 0, end = 0;
    std::string name;
    bool system = false;  // ntdll, win32u, kernelbase, ...: system calls are named from its exports
    bool exports_loaded = false;
    std::vector<Export> exports;  // sorted by rva
};
std::vector<Module> g_modules;  // sorted by base
uintptr_t g_self_base = 0, g_self_end = 0, g_exe_base = 0, g_exe_end = 0;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void RefreshModules() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 1024);
    std::vector<Module> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(mods[i], path, MAX_PATH);
        std::string name = len ? std::string(path, len) : std::string("?");
        size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        Module m;
        m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        m.end = m.base + mi.SizeOfImage;
        m.name = name;
        const std::string l = Lower(name);
        m.system = l == "ntdll.dll" || l == "win32u.dll" || l == "kernelbase.dll" || l == "kernel32.dll" ||
                   l == "user32.dll" || l == "gdi32.dll";
        for (const Module& old : g_modules)
            if (old.base == m.base && old.name == m.name && old.exports_loaded) {
                m.exports = old.exports;
                m.exports_loaded = true;
            }
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    g_modules.swap(out);
}

Module* ModuleAt(uintptr_t p) {
    auto it = std::upper_bound(g_modules.begin(), g_modules.end(), p,
                               [](uintptr_t v, const Module& m) { return v < m.base; });
    if (it == g_modules.begin()) return nullptr;
    --it;
    return p < it->end ? &*it : nullptr;
}

void LoadExports(Module& m) {
    m.exports_loaded = true;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(m.base + dir.VirtualAddress);
    auto* funcs = reinterpret_cast<const DWORD*>(m.base + ed->AddressOfFunctions);
    auto* names = reinterpret_cast<const DWORD*>(m.base + ed->AddressOfNames);
    auto* ords = reinterpret_cast<const WORD*>(m.base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        if (ords[i] >= ed->NumberOfFunctions) continue;
        const DWORD rva = funcs[ords[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;  // forwarded
        m.exports.push_back({rva, reinterpret_cast<const char*>(m.base + names[i])});
    }
    std::sort(m.exports.begin(), m.exports.end(), [](const Export& a, const Export& b) { return a.rva < b.rva; });
}

const std::string* ExportAt(Module& m, uintptr_t p) {
    if (!m.exports_loaded) LoadExports(m);
    const uint32_t rva = static_cast<uint32_t>(p - m.base);
    auto it = std::upper_bound(m.exports.begin(), m.exports.end(), rva,
                               [](uint32_t v, const Export& e) { return v < e.rva; });
    if (it == m.exports.begin()) return nullptr;
    --it;
    return rva - it->rva < 0x400 ? &it->name : nullptr;
}

// --------------------------------------------- the mod's own symbol names --
struct Symbol {
    uint32_t rva;
    std::string name;
};
std::vector<Symbol> g_symbols;  // sorted by rva; empty for a DLL without a symbol table

// "holevr::vr::controls::(anonymous namespace)::OnTick(unsigned long long, float)"
// -> "controls::OnTick"
std::string ShortName(const char* raw) {
    int status = 0;
    char* d = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    std::string s = (status == 0 && d) ? d : raw;
    std::free(d);
    auto erase_all = [&](const std::string& what) {
        for (size_t at; (at = s.find(what)) != std::string::npos;) s.erase(at, what.size());
    };
    erase_all("holevr::");
    erase_all("(anonymous namespace)::");
    erase_all("vr::");
    // Drop the parameter list (a lambda is shown as the function it is in).
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{' || s[i] == '<') ++depth;
        else if (s[i] == '}' || s[i] == '>') --depth;
        else if (s[i] == '(' && depth == 0 && !(i >= 8 && s.compare(i - 8, 8, "operator") == 0)) {
            s.resize(i);
            break;
        }
    }
    if (s.size() > 90) s = s.substr(0, 87) + "...";
    return s;
}

void LoadOwnSymbols() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadOwnSymbols), &self))
        return;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + got);
    fclose(f);
    if (file.size() < 0x400) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    const IMAGE_FILE_HEADER& fh = nt->FileHeader;
    const DWORD sym_at = fh.PointerToSymbolTable, nsym = fh.NumberOfSymbols;
    if (!sym_at || !nsym || sym_at + static_cast<uint64_t>(nsym) * 18 + 4 > file.size()) return;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) + fh.SizeOfOptionalHeader);
    const uint8_t* syms = file.data() + sym_at;
    const char* strtab = reinterpret_cast<const char*>(syms + nsym * 18ull);
    const size_t strtab_size = file.size() - (sym_at + nsym * 18ull);
    std::vector<Symbol> out;
    for (DWORD i = 0; i < nsym; ++i) {
        const uint8_t* s = syms + i * 18ull;
        uint32_t value;
        int16_t section;
        uint16_t type;
        std::memcpy(&value, s + 8, 4);
        std::memcpy(&section, s + 12, 2);
        std::memcpy(&type, s + 14, 2);
        const uint8_t storage = s[16], aux = s[17];
        if (section > 0 && section <= fh.NumberOfSections && (type & 0x30) == 0x20 && (storage == 2 || storage == 3)) {
            std::string raw;
            uint32_t zero, offset;
            std::memcpy(&zero, s, 4);
            std::memcpy(&offset, s + 4, 4);
            if (zero) raw.assign(reinterpret_cast<const char*>(s), strnlen(reinterpret_cast<const char*>(s), 8));
            else if (offset < strtab_size) raw = strtab + offset;
            if (!raw.empty()) out.push_back({sections[section - 1].VirtualAddress + value, raw});
        }
        i += aux;
    }
    std::sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
    g_symbols.swap(out);
}

std::unordered_map<uint32_t, std::string> g_name_cache;

std::string ModFunctionName(uint32_t rva) {
    auto c = g_name_cache.find(rva);
    if (c != g_name_cache.end()) return c->second;
    std::string name;
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), rva,
                               [](uint32_t v, const Symbol& s) { return v < s.rva; });
    if (it != g_symbols.begin()) {
        --it;
        name = ShortName(it->name.c_str());
    }
    if (name.empty()) {
        char b[32];
        std::snprintf(b, sizeof(b), "dxgi+0x%x", rva);
        name = b;
    }
    g_name_cache[rva] = name;
    return name;
}

// ------------------------------------------------------------- targets --
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

struct ThreadBasicInfo {
    LONG exit_status;
    PVOID teb;
    ULONG_PTR process, thread;
    ULONG_PTR affinity;
    LONG priority, base_priority;
};

constexpr size_t kStackCopy = 512 * 1024;
constexpr size_t kStackPad = 512 * 1024;  // reads past the copy find stale data, never unmapped memory
constexpr int kMaxFrames = 96;

// What one sample's frames say, per mod function.
struct FnStat {
    long total = 0;    // on the stack (inclusive)
    long own = 0;      // innermost mod function, running its own code
    long engine = 0;   // innermost mod function, inside game code it called
    long syscall = 0;  // innermost mod function, inside a system call it made
    long other = 0;    // innermost mod function, inside another DLL (driver, CRT)
};

struct Target {
    const char* label = "";
    DWORD tid = 0;
    HANDLE h = nullptr;
    uintptr_t teb = 0, stack_base = 0;
    uint8_t* buf = nullptr;
    // this window
    long samples = 0, failed = 0, in_mod = 0, waiting = 0;
    std::unordered_map<uint32_t, FnStat> fns;          // key: mod function rva
    std::unordered_map<std::string, long> syscalls;    // system call name -> samples
    std::unordered_map<std::string, long> mod_syscalls;  // system calls made directly by mod code
    std::unordered_map<std::string, long> leaf_modules;  // module the thread was in, outside waits
    std::unordered_map<uint32_t, long> exe_leaf;       // game exe function rva (innermost) -> samples, outside waits
};

Target g_targets[4];  // 0 game, 1 render, 2 rhi, 3 present (when not one of the others)
GetThreadDescriptionFn g_get_desc = nullptr;
NtQueryInformationThreadFn g_query_thread = nullptr;

bool OpenTarget(Target& t, DWORD tid, const char* label) {
    if (t.h) {
        CloseHandle(t.h);
        t.h = nullptr;
    }
    t.tid = 0;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return false;
    ThreadBasicInfo tbi{};
    if (!g_query_thread || g_query_thread(h, 0, &tbi, sizeof(tbi), nullptr) < 0 || !tbi.teb) {
        CloseHandle(h);
        return false;
    }
    t.teb = reinterpret_cast<uintptr_t>(tbi.teb);
    t.stack_base = *reinterpret_cast<const volatile uintptr_t*>(t.teb + 0x08);
    if (!t.buf)
        t.buf = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kStackCopy + kStackPad, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!t.buf) {
        CloseHandle(h);
        return false;
    }
    t.h = h;
    t.tid = tid;
    t.label = label;
    log::Info("profile: sampling the %s (thread %lu)", label, static_cast<unsigned long>(tid));
    return true;
}

// Finds the engine's render and RHI threads by the names the engine gives them.
void FindNamedThreads() {
    if (!g_get_desc) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    DWORD render = 0, rhi = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR desc = nullptr;
        if (SUCCEEDED(g_get_desc(h, &desc)) && desc) {
            std::wstring d(desc);
            LocalFree(desc);
            if (!render && d.rfind(L"RenderThread", 0) == 0) render = te.th32ThreadID;
            if (!rhi && d == L"RHIThread") rhi = te.th32ThreadID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    if (render && g_targets[1].tid != render) OpenTarget(g_targets[1], render, "render thread");
    if (rhi && g_targets[2].tid != rhi) OpenTarget(g_targets[2], rhi, "RHI thread");
    // The present thread is sampled on its own only when it is none of these.
    Target& p = g_targets[3];
    if (p.h && (p.tid == g_targets[0].tid || p.tid == g_targets[1].tid || p.tid == g_targets[2].tid)) {
        CloseHandle(p.h);
        p.h = nullptr;
        p.tid = 0;
    }
}

DWORD64 Reg(const CONTEXT& c, int i) {
    switch (i) {
        case 0: return c.Rax;
        case 1: return c.Rcx;
        case 2: return c.Rdx;
        case 3: return c.Rbx;
        case 4: return c.Rsp;
        case 5: return c.Rbp;
        case 6: return c.Rsi;
        case 7: return c.Rdi;
        case 8: return c.R8;
        case 9: return c.R9;
        case 10: return c.R10;
        case 11: return c.R11;
        case 12: return c.R12;
        case 13: return c.R13;
        case 14: return c.R14;
        default: return c.R15;
    }
}

// Unwinds a stack copy. Registers that point into the original stack are
// moved to the copy before each step, so every read stays inside our buffer.
int Unwind(CONTEXT c, uintptr_t orig_lo, size_t copied, uint8_t* buf, uintptr_t* frames) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(buf), hi = lo + copied;
    const uintptr_t orig_hi = orig_lo + copied;
    const intptr_t delta = static_cast<intptr_t>(lo) - static_cast<intptr_t>(orig_lo);
    auto reloc = [&](DWORD64& r) {
        if (r >= orig_lo && r < orig_hi) r = static_cast<DWORD64>(static_cast<intptr_t>(r) + delta);
    };
    auto reloc_all = [&] {
        reloc(c.Rbx);
        reloc(c.Rbp);
        reloc(c.Rsi);
        reloc(c.Rdi);
        reloc(c.R12);
        reloc(c.R13);
        reloc(c.R14);
        reloc(c.R15);
    };
    int n = 0;
    frames[n++] = c.Rip;
    if (!copied) return n;
    c.Rsp = static_cast<DWORD64>(static_cast<intptr_t>(c.Rsp) + delta);
    reloc_all();
    while (n < kMaxFrames) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!rf) {
            if (c.Rsp < lo || c.Rsp + 8 > hi) break;
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
        } else {
            const auto* ui = reinterpret_cast<const uint8_t*>(image_base + rf->UnwindData);
            const int freg = ui[3] & 0x0F;
            if (freg) {
                const DWORD64 v = Reg(c, freg);
                if (v < lo || v >= hi) break;
            }
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
        }
        if (c.Rsp < lo || c.Rsp >= hi || !c.Rip) break;
        reloc_all();
        if (!ModuleAt(c.Rip)) break;
        frames[n++] = c.Rip;
    }
    return n;
}

bool IsWaitName(const std::string& s) {
    return s.find("Wait") != std::string::npos || s.find("Delay") != std::string::npos ||
           s.find("RemoveIoCompletion") != std::string::npos || s.find("Yield") != std::string::npos;
}

// Leaf right after a "syscall" instruction in ntdll/win32u: the thread is in
// the kernel. Returns the system call's name, or empty.
std::string SystemCallAt(uintptr_t rip, Module* m) {
    if (!m || !m->system || rip < m->base + 2) return {};
    const auto* code = reinterpret_cast<const uint8_t*>(rip - 2);
    if (!(code[0] == 0x0F && code[1] == 0x05)) return {};
    const std::string* e = ExportAt(*m, rip);
    return e ? *e : m->name + "+syscall";
}

void Sample(Target& t) {
    if (!t.h) return;
    alignas(16) CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    size_t copied = 0;
    // ---- the thread is stopped from here: no locks, no allocation ----
    if (SuspendThread(t.h) == static_cast<DWORD>(-1)) {
        ++t.failed;
        if (t.failed > 50 && !t.samples) {  // the thread is gone
            CloseHandle(t.h);
            t.h = nullptr;
            t.tid = 0;
        }
        return;
    }
    const bool got = GetThreadContext(t.h, &ctx) != 0;
    if (got) {
        const uintptr_t limit = *reinterpret_cast<const volatile uintptr_t*>(t.teb + 0x10);
        if (ctx.Rsp >= limit && ctx.Rsp < t.stack_base) {
            copied = std::min<size_t>(t.stack_base - ctx.Rsp, kStackCopy);
            std::memcpy(t.buf, reinterpret_cast<const void*>(ctx.Rsp), copied);
        }
    }
    ResumeThread(t.h);
    // ---- running again ----
    if (!got) {
        ++t.failed;
        return;
    }
    uintptr_t frames[kMaxFrames];
    const int n = Unwind(ctx, ctx.Rsp, copied, t.buf, frames);
    ++t.samples;

    Module* leaf_mod = ModuleAt(frames[0]);
    const std::string sys = SystemCallAt(frames[0], leaf_mod);
    const bool waiting = !sys.empty() && IsWaitName(sys);
    if (!sys.empty()) t.syscalls[sys]++;
    if (waiting) ++t.waiting;

    // Mod frames, leaf first.
    uint32_t mod_fns[kMaxFrames];
    int nm = 0;
    int innermost_at = -1;
    for (int i = 0; i < n; ++i) {
        if (frames[i] < g_self_base || frames[i] >= g_self_end) continue;
        DWORD64 image_base = 0;
        // Return addresses point after the call; step back one byte so a call
        // at the very end of a function still finds that function.
        const uintptr_t pc = i == 0 ? frames[i] : frames[i] - 1;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        const uint32_t rva = rf ? rf->BeginAddress : static_cast<uint32_t>(pc - g_self_base);
        if (innermost_at < 0) innermost_at = i;
        bool seen = false;
        for (int k = 0; k < nm; ++k) seen |= mod_fns[k] == rva;
        if (!seen) mod_fns[nm++] = rva;
    }
    if (nm) ++t.in_mod;
    for (int k = 0; k < nm; ++k) t.fns[mod_fns[k]].total++;
    if (nm) {
        FnStat& in = t.fns[mod_fns[0]];
        if (innermost_at == 0) {
            in.own++;
        } else {
            // The frame just inside the mod's call says what it called into.
            Module* callee = ModuleAt(frames[innermost_at - 1]);
            if (callee && callee->base == g_exe_base) {
                in.engine++;
            } else if (callee && callee->system) {
                in.syscall++;
                t.mod_syscalls[sys.empty() ? callee->name : sys]++;
            } else {
                in.other++;
            }
        }
    }
    if (!waiting) {
        t.leaf_modules[leaf_mod ? leaf_mod->name : std::string("?")]++;
        if (leaf_mod && leaf_mod->base == g_exe_base) {
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(frames[0], &image_base, nullptr);
            t.exe_leaf[rf ? rf->BeginAddress : static_cast<uint32_t>(frames[0] - g_exe_base)]++;
        }
    }
}

template <typename Map>
std::vector<std::pair<typename Map::key_type, long>> Top(const Map& m, size_t k) {
    std::vector<std::pair<typename Map::key_type, long>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > k) v.resize(k);
    return v;
}

// Appends "item" to the line, logging the line first when it would get too long.
void Emit(std::string& line, const std::string& head, const std::string& item) {
    if (line.size() + item.size() > 1700) {
        log::Info("%s", line.c_str());
        line = head + " (cont.)";
    }
    line += item;
}

void Report(Target& t, double span_ms, long frames, const char* frame_kind) {
    if (!t.samples) return;
    const double ms_per_sample = frames > 0 ? span_ms / frames / t.samples : 0.0;  // ms per frame for one sample
    const double s = static_cast<double>(t.samples);
    auto pct = [&](long c) { return 100.0 * c / s; };
    auto mspf = [&](long c) { return c * ms_per_sample; };
    char b[600];
    std::snprintf(b, sizeof(b),
                  "profile %s: %ld samples in %.1f s, %ld %s frames (%.2f ms each); busy %.0f%% (%.2f ms a frame), "
                  "waiting %.0f%%; in mod code or what it called %.0f%% (%.2f ms a frame)",
                  t.label, t.samples, span_ms / 1000.0, frames, frame_kind, frames > 0 ? span_ms / frames : 0.0,
                  pct(t.samples - t.waiting), mspf(t.samples - t.waiting), pct(t.waiting), pct(t.in_mod),
                  mspf(t.in_mod));
    log::Info("%s", b);

    const std::string head_total = std::string("profile ") + t.label + ", mod functions incl. all they call:";
    std::string line = head_total;
    // Merge entries that symbolize to the same name (hot and cold parts).
    std::unordered_map<std::string, FnStat> by_name;
    for (const auto& [rva, st] : t.fns) {
        FnStat& m = by_name[ModFunctionName(rva)];
        m.total = std::max(m.total, st.total);  // the parts of one function are on the stack together
        m.own += st.own;
        m.engine += st.engine;
        m.syscall += st.syscall;
        m.other += st.other;
    }
    std::vector<std::pair<std::string, FnStat>> fns(by_name.begin(), by_name.end());
    std::sort(fns.begin(), fns.end(), [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
    int shown = 0;
    for (const auto& [name, st] : fns) {
        if (shown >= 28 || pct(st.total) < 0.3) break;
        std::snprintf(b, sizeof(b), " | %s %.2f ms (%.1f%%)", name.c_str(), mspf(st.total), pct(st.total));
        Emit(line, head_total, b);
        ++shown;
    }
    if (shown) log::Info("%s", line.c_str());

    std::sort(fns.begin(), fns.end(), [](const auto& a, const auto& b) {
        const auto d = [](const FnStat& s) { return s.own + s.engine + s.syscall + s.other; };
        return d(a.second) > d(b.second);
    });
    const std::string head_direct =
        std::string("profile ") + t.label + ", direct cost (own code / game calls / Windows calls / other DLLs):";
    line = head_direct;
    shown = 0;
    for (const auto& [name, st] : fns) {
        const long d = st.own + st.engine + st.syscall + st.other;
        if (shown >= 24 || pct(d) < 0.3) break;
        std::snprintf(b, sizeof(b), " | %s %.2f ms = %.2f / %.2f / %.2f / %.2f", name.c_str(), mspf(d), mspf(st.own),
                      mspf(st.engine), mspf(st.syscall), mspf(st.other));
        Emit(line, head_direct, b);
        ++shown;
    }
    if (shown) log::Info("%s", line.c_str());

    const std::string head_sys = std::string("profile ") + t.label + ", system calls (share of all samples; ms a frame made from mod code):";
    line = head_sys;
    shown = 0;
    for (const auto& [name, c] : Top(t.syscalls, 10)) {
        auto it = t.mod_syscalls.find(name);
        const long from_mod = it == t.mod_syscalls.end() ? 0 : it->second;
        std::snprintf(b, sizeof(b), " | %s %.1f%%; %.2f ms", name.c_str(), pct(c), mspf(from_mod));
        Emit(line, head_sys, b);
        ++shown;
    }
    if (shown) log::Info("%s", line.c_str());

    const std::string head_mod = std::string("profile ") + t.label + ", busy time by module:";
    line = head_mod;
    for (const auto& [name, c] : Top(t.leaf_modules, 8)) {
        std::snprintf(b, sizeof(b), " | %s %.2f ms (%.0f%%)", name.c_str(), mspf(c), pct(c));
        Emit(line, head_mod, b);
    }
    std::string exe = " | hottest game functions:";
    for (const auto& [rva, c] : Top(t.exe_leaf, 6)) {
        std::snprintf(b, sizeof(b), " exe+0x%x %.2f ms", rva, mspf(c));
        exe += b;
    }
    Emit(line, head_mod, exe);
    log::Info("%s", line.c_str());
}

void ResetWindow(Target& t) {
    t.samples = t.failed = t.in_mod = t.waiting = 0;
    t.fns.clear();
    t.syscalls.clear();
    t.mod_syscalls.clear();
    t.leaf_modules.clear();
    t.exe_leaf.clear();
}

DWORD WINAPI SamplerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (g_get_desc) {
        using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
            set(GetCurrentThread(), L"HOLE VR profiler");
    }
    LoadOwnSymbols();
    RefreshModules();
    log::Info("profile: on, %d samples a second per thread; mod function names %s", config::GetInt("profile_hz", 500),
              g_symbols.empty() ? "as dxgi+offsets (this DLL has no symbol table)" : "from the DLL's symbol table");
    const int hz = std::clamp(config::GetInt("profile_hz", 500), 20, 2000);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const LONGLONG period_100ns = 10000000LL / hz;
    double window_start = perf::NowMs();
    long game_frames0 = g_game_frames.load(), present_frames0 = g_present_frames.load();
    DWORD last_refresh = GetTickCount();
    for (;;) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -period_100ns;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(1000 / hz > 0 ? 1000 / hz : 1);
        }
        const DWORD gt = g_game_tid.load(), pt = g_present_tid.load();
        if (gt && g_targets[0].tid != gt) OpenTarget(g_targets[0], gt, "game thread");
        if (pt && pt != g_targets[0].tid && pt != g_targets[1].tid && pt != g_targets[2].tid && g_targets[3].tid != pt)
            OpenTarget(g_targets[3], pt, "present thread");
        for (Target& t : g_targets) Sample(t);

        const DWORD now_tick = GetTickCount();
        if (now_tick - last_refresh > 5000) {
            last_refresh = now_tick;
            RefreshModules();
            FindNamedThreads();
        }
        const double now = perf::NowMs();
        if (now - window_start >= 10000.0) {
            const long gf = g_game_frames.load(), pf = g_present_frames.load();
            const double span = now - window_start;
            const long game_frames = gf - game_frames0, present_frames = pf - present_frames0;
            if (game_frames > 0 || present_frames > 0) {
                Report(g_targets[0], span, game_frames, "game");
                Report(g_targets[1], span, present_frames, "presented");
                Report(g_targets[2], span, present_frames, "presented");
                Report(g_targets[3], span, present_frames, "presented");
            }
            for (Target& t : g_targets) ResetWindow(t);
            window_start = now;
            game_frames0 = gf;
            present_frames0 = pf;
        }
    }
    return 0;
}

}  // namespace

void Start() {
    if (!config::GetBool("profile", true) || g_started.exchange(true)) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Start), &self);
    MODULEINFO mi{};
    if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
        g_self_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        g_self_end = g_self_base + mi.SizeOfImage;
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe && GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi))) {
        g_exe_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        g_exe_end = g_exe_base + mi.SizeOfImage;
    }
    g_get_desc = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    g_query_thread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!g_self_base || !g_query_thread) {
        log::Warn("profile: could not start (module or thread information unavailable)");
        return;
    }
    if (HANDLE t = CreateThread(nullptr, 0, &SamplerThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void NoteGameFrame() {
    static thread_local bool named = false;
    if (!named) {
        named = true;
        g_game_tid.store(GetCurrentThreadId());
    }
    g_game_frames.fetch_add(1, std::memory_order_relaxed);
}

void NotePresentThread() {
    static thread_local bool named = false;
    if (!named) {
        named = true;
        g_present_tid.store(GetCurrentThreadId());
    }
    g_present_frames.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace holevr::sampler
