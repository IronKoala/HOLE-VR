#include "ue/process_event.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/log.h"
#include "core/perf.h"
#include "core/memory.h"
#include "ue/reflection.h"

namespace holevr::ue::process_event {
namespace {

// EFunctionFlags bits used to identify fields and code.
constexpr uint32_t FUNC_Native = 0x00000400;
constexpr uint32_t FUNC_Event = 0x00000800;
constexpr uint32_t FUNC_BlueprintCallable = 0x04000000;
constexpr uint32_t FUNC_BlueprintEvent = 0x08000000;

uintptr_t g_flags_off = 0;
int g_index = -1;
bool g_ready = false;

using ProcessEventFn = void (*)(UObject self, UObject function, void* params);

// One entry per patched vtable. Lookups are lock-free reads; entries are
// only appended (from the init thread) before the slot is patched.
struct PatchedTable {
    uintptr_t vtable = 0;
    ProcessEventFn original = nullptr;
};
constexpr int kMaxTables = 16;
PatchedTable g_tables[kMaxTables];
std::atomic<int> g_table_count{0};

struct Callback {
    UObject function = 0;
    PreFn pre = nullptr;
    PostFn post = nullptr;
};
constexpr int kMaxCallbacks = 64;
Callback g_callbacks[kMaxCallbacks];
std::atomic<int> g_callback_count{0};
std::atomic<PostFn> g_any{nullptr};

const mem::Section* Text() { return mem::Game().Find(".text"); }

bool InText(uintptr_t p) {
    const mem::Section* t = Text();
    return t && p >= t->start && p < t->start + t->size;
}

// Follows `jmp rel32` / `jmp [rip+x]` thunks the linker sometimes puts in vtables.
uintptr_t FollowThunks(uintptr_t fn) {
    for (int i = 0; i < 4 && InText(fn); ++i) {
        uint8_t b[6];
        if (!mem::Read(fn, b)) break;
        if (b[0] == 0xE9) {
            int32_t rel;
            std::memcpy(&rel, b + 1, 4);
            fn = fn + 5 + rel;
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            int32_t rel;
            std::memcpy(&rel, b + 2, 4);
            fn = mem::ReadOr<uintptr_t>(fn + 6 + rel);
        } else {
            break;
        }
    }
    return fn;
}

// UFunction::FunctionFlags sits right after UStruct. Find it by checking two
// functions every UE5 build has: one native and callable, one a Blueprint event.
bool FindFlagsOffset() {
    UObject native_fn = FindObject("Function /Script/Engine.Actor:K2_GetActorLocation");
    UObject event_fn = FindObject("Function /Script/Engine.Actor:ReceiveBeginPlay");
    if (!native_fn || !event_fn) return false;
    for (uintptr_t o = 0x90; o <= 0xD0; o += 4) {
        uint32_t a = mem::ReadOr<uint32_t>(native_fn + o);
        uint32_t b = mem::ReadOr<uint32_t>(event_fn + o);
        bool a_ok = (a & FUNC_Native) && (a & FUNC_BlueprintCallable) && !(a & FUNC_Event);
        bool b_ok = (b & FUNC_Event) && (b & FUNC_BlueprintEvent) && !(b & FUNC_Native);
        if (a_ok && b_ok) {
            g_flags_off = o;
            return true;
        }
    }
    return false;
}

// ProcessEvent tests `Function->FunctionFlags & FUNC_Native` and
// `& FUNC_HasOutParms` early on. Look for those tests (dword or byte forms).
bool LooksLikeProcessEvent(uintptr_t fn) {
    constexpr size_t kScan = 0x400;
    if (!InText(fn) || !mem::IsReadable(fn, kScan)) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(fn);
    const uint32_t off = static_cast<uint32_t>(g_flags_off);
    bool native_test = false, outparms_test = false;
    for (size_t i = 0; i + 10 <= kScan; ++i) {
        uint32_t disp, imm;
        if (p[i] == 0xF7 && (p[i + 1] & 0xC0) == 0x80) {  // test dword [reg+disp32], imm32
            std::memcpy(&disp, p + i + 2, 4);
            std::memcpy(&imm, p + i + 6, 4);
            if (disp == off && imm == FUNC_Native) native_test = true;
            if (disp == off && imm == 0x00400000) outparms_test = true;
        } else if (p[i] == 0xF6 && (p[i + 1] & 0xC0) == 0x80) {  // test byte [reg+disp32], imm8
            std::memcpy(&disp, p + i + 2, 4);
            if (disp == off + 1 && p[i + 6] == 0x04) native_test = true;
            if (disp == off + 2 && p[i + 6] == 0x40) outparms_test = true;
        }
        if (native_test && outparms_test) return true;
    }
    return false;
}

// Prologue of UObject::ProcessEvent in UE 5.x shipping builds (MSVC).
constexpr const char* kProcessEventPattern =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 ?? ?? ?? ?? 8B 41 0C 45 33 F6 3B 05 ?? ?? ?? ?? 4D 8B "
    "F8 48 8B F2 4C 8B E1 41 B8 FF FF 00 00 0F 8D";

bool FindVtableIndex() {
    UObject cdo = FindObject("Object /Script/CoreUObject.Default__Object");
    if (!cdo) return false;
    uintptr_t vt = mem::ReadOr<uintptr_t>(cdo);
    if (!vt) return false;

    uintptr_t by_pattern = 0;
    if (const mem::Section* t = Text()) by_pattern = mem::FindPattern(t->start, t->size, kProcessEventPattern);

    int heuristic_index = -1, pattern_index = -1;
    for (int i = 0; i < 256; ++i) {
        uintptr_t fn = mem::ReadOr<uintptr_t>(vt + i * 8ull);
        if (!InText(fn)) break;  // past the end of the table
        uintptr_t target = FollowThunks(fn);
        if (by_pattern && target == by_pattern && pattern_index < 0) pattern_index = i;
        if (heuristic_index < 0 && LooksLikeProcessEvent(target)) heuristic_index = i;
    }
    log::Info("process_event: vtable index by code check %d, by pattern %d (pattern at %s)",
              heuristic_index, pattern_index, by_pattern ? "found" : "not found");
    if (pattern_index >= 0 && heuristic_index >= 0 && pattern_index != heuristic_index)
        log::Warn("process_event: the two methods disagree; using the pattern result");
    g_index = pattern_index >= 0 ? pattern_index : heuristic_index;
    return g_index >= 0;
}

ProcessEventFn OriginalFor(uintptr_t vtable) {
    int n = g_table_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_tables[i].vtable == vtable) return g_tables[i].original;
    return nullptr;
}

thread_local int t_depth = 0;
perf::Counter g_hook_perf{"ProcessEvent callbacks"};
std::atomic<bool> g_hook_perf_registered{false};

void Hook(UObject self, UObject function, void* params) {
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(self);
    ProcessEventFn original = OriginalFor(vt);
    if (!original) return;  // cannot happen: only patched tables point here
    // Do not re-enter our callbacks from calls our callbacks make, and keep
    // the hook transparent if something deep in the engine recurses a lot.
    if (t_depth > 0) {
        original(self, function, params);
        return;
    }
    if (!g_hook_perf_registered.exchange(true)) perf::Register(&g_hook_perf);
    double t_pre = perf::NowMs();
    ++t_depth;
    const Callback* match = nullptr;
    int n = g_callback_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_callbacks[i].function == function) {
            match = &g_callbacks[i];
            break;
        }
    }
    bool run = true;
    if (match && match->pre) run = match->pre(self, function, params);
    --t_depth;
    double t_mid = perf::NowMs();

    if (run) original(self, function, params);

    double t_post = perf::NowMs();
    ++t_depth;
    if (PostFn any = g_any.load(std::memory_order_acquire)) any(self, function, params);
    if (match && match->post) match->post(self, function, params);
    --t_depth;
    g_hook_perf.Add((t_mid - t_pre) + (perf::NowMs() - t_post));
}

}  // namespace

bool Init() {
    if (g_ready) return true;
    if (!reflection::Ready()) return false;
    if (!g_flags_off && !FindFlagsOffset()) return false;
    if (g_index < 0 && !FindVtableIndex()) {
        log::Error("process_event: could not find UObject::ProcessEvent");
        return false;
    }
    g_ready = true;
    log::Info("process_event: FunctionFlags at 0x%llx, ProcessEvent is vtable slot %d",
              static_cast<unsigned long long>(g_flags_off), g_index);
    return true;
}

bool Ready() { return g_ready; }
int VtableIndex() { return g_index; }
uintptr_t FunctionFlagsOffset() { return g_flags_off; }

void Invoke(UObject obj, UObject function, void* params) {
    if (!g_ready || !obj || !function) return;
    uintptr_t vt = mem::ReadOr<uintptr_t>(obj);
    // Call the original for patched tables so our own calls skip the hook.
    ProcessEventFn fn = OriginalFor(vt);
    if (!fn) fn = reinterpret_cast<ProcessEventFn>(mem::ReadOr<uintptr_t>(vt + g_index * 8ull));
    if (!fn) return;
    fn(obj, function, params);
}

bool HookVtableOf(UObject sample) {
    if (!g_ready || !sample) return false;
    uintptr_t vt = mem::ReadOr<uintptr_t>(sample);
    if (!vt) return false;
    if (OriginalFor(vt)) return true;
    int n = g_table_count.load();
    if (n >= kMaxTables) return false;
    uintptr_t slot = vt + g_index * 8ull;
    auto original = reinterpret_cast<ProcessEventFn>(mem::ReadOr<uintptr_t>(slot));
    if (!original) return false;
    g_tables[n] = {vt, original};
    g_table_count.store(n + 1, std::memory_order_release);

    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 8, PAGE_READWRITE, &old)) {
        log::Error("process_event: VirtualProtect failed on vtable %p", reinterpret_cast<void*>(vt));
        return false;
    }
    auto hook = reinterpret_cast<uintptr_t>(&Hook);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(hook));
    VirtualProtect(reinterpret_cast<void*>(slot), 8, old, &old);
    log::Info("process_event: hooked vtable exe+0x%llx (%s)",
              static_cast<unsigned long long>(vt - mem::Game().base), GetFullName(GetClass(sample)).c_str());
    return true;
}

bool On(UObject function, PreFn pre, PostFn post) {
    if (!function) return false;
    int n = g_callback_count.load();
    for (int i = 0; i < n; ++i) {
        if (g_callbacks[i].function == function) {
            g_callbacks[i].pre = pre;
            g_callbacks[i].post = post;
            return true;
        }
    }
    if (n >= kMaxCallbacks) return false;
    g_callbacks[n] = {function, pre, post};
    g_callback_count.store(n + 1, std::memory_order_release);
    return true;
}

void OnAny(PostFn post) { g_any.store(post, std::memory_order_release); }

}  // namespace holevr::ue::process_event
