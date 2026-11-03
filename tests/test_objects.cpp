// Native tests for the object scan and FindObject (no game, no Windows).
//   g++ -std=c++20 -Isrc -Itests/win_shim tests/test_objects.cpp -o /tmp/test_objects && /tmp/test_objects
//
// Builds a fake name pool and object array in this process's memory, points
// the real code at them, and checks that FindObject finds exactly what the
// old string-by-string scan found, including numbered names ("Foo_3").
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"

// The code under test, in this file so the test can set its internals.
#include "core/memory.cpp"
#include "ue/names.cpp"
#include "ue/objects.cpp"

namespace holevr::log {
void Info(const char*, ...) {}
void Warn(const char*, ...) {}
void Error(const char*, ...) {}
}  // namespace holevr::log

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

// Fake memory map: 64 KB granules, readable unless listed in g_bad.
static std::set<uintptr_t> g_bad;
static int g_queries = 0;
SIZE_T VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* mbi, SIZE_T) {
    ++g_queries;
    uintptr_t base = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(0xFFFF);
    mbi->BaseAddress = reinterpret_cast<void*>(base);
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 0x10000;
    bool bad = g_bad.count(base) != 0;
    mbi->State = bad ? MEM_RESERVE : MEM_COMMIT;
    mbi->Protect = bad ? PAGE_NOACCESS : PAGE_READWRITE;
    return sizeof(*mbi);
}

using namespace holevr;
using namespace holevr::ue;

static void* AlignedAlloc(size_t bytes) {
    void* p = std::aligned_alloc(0x10000, (bytes + 0xFFFF) & ~size_t(0xFFFF));
    std::memset(p, 0, (bytes + 0xFFFF) & ~size_t(0xFFFF));
    return p;
}

// ------------------------------------------------------------ name pool --
static unsigned char* g_block;
static uint32_t g_cursor = 0;
static std::vector<unsigned char> g_pool_mem(0x10 + 8192 * 8);

static uint32_t AddName(const std::string& s) {
    uint32_t off = g_cursor;
    uint16_t raw = static_cast<uint16_t>(s.size() << 6);  // narrow, no hash bits
    std::memcpy(g_block + off, &raw, 2);
    std::memcpy(g_block + off + 2, s.data(), s.size());
    g_cursor += (2 + s.size() + 1) & ~1u;
    std::memcpy(g_pool_mem.data() + 0x0C, &g_cursor, 4);
    return off / 2;  // block 0
}

// -------------------------------------------------------------- objects --
static unsigned char* g_heap;
static size_t g_heap_used = 0;
static std::vector<uintptr_t> g_chunk_ptrs(4);
static std::vector<unsigned char> g_array_hdr(0x30);

static uintptr_t NewObject(int index, uintptr_t cls, FName name, uintptr_t outer, unsigned char* where = nullptr) {
    uintptr_t o = reinterpret_cast<uintptr_t>(where ? where : g_heap + g_heap_used);
    if (!where) g_heap_used += 0x40;
    int32_t idx = index;
    std::memcpy(reinterpret_cast<void*>(o + 0x0C), &idx, 4);
    std::memcpy(reinterpret_cast<void*>(o + 0x10), &cls, 8);
    std::memcpy(reinterpret_cast<void*>(o + 0x18), &name, 8);
    std::memcpy(reinterpret_cast<void*>(o + 0x20), &outer, 8);
    // Slot in the chunked array.
    uintptr_t chunk = g_chunk_ptrs[index / 65536];
    std::memcpy(reinterpret_cast<void*>(chunk + (index % 65536) * 24), &o, 8);
    return o;
}

static void SetClass(uintptr_t o, uintptr_t cls) { std::memcpy(reinterpret_cast<void*>(o + 0x10), &cls, 8); }

static FName N(uint32_t index, uint32_t number = 0) { return FName{index, number}; }

// The scan FindObject used before: every object's name built as a string.
static UObject OldFindObject(const std::string& full_name) {
    size_t space = full_name.find(' ');
    if (space == std::string::npos) return 0;
    size_t last = full_name.find_last_of(".:");
    std::string short_name = full_name.substr(last == std::string::npos ? space + 1 : last + 1);
    for (int i = 0; i < objects::Count(); ++i) {
        UObject o = objects::Get(i);
        if (!o || GetName(o) != short_name) continue;
        if (GetFullName(o) == full_name) return o;
    }
    return 0;
}

int main() {
    // Name pool with "None" and "ByteProperty" first, like the engine's.
    g_block = static_cast<unsigned char*>(AlignedAlloc(0x20000));
    uintptr_t block_ptr = reinterpret_cast<uintptr_t>(g_block);
    std::memcpy(g_pool_mem.data() + 0x10, &block_ptr, 8);
    AddName("None");
    AddName("ByteProperty");
    uint32_t n_class = AddName("Class"), n_package = AddName("Package"), n_function = AddName("Function"),
             n_bpgc = AddName("BlueprintGeneratedClass"), n_core = AddName("/Script/CoreUObject"),
             n_engine = AddName("/Script/Engine"), n_character = AddName("Character"),
             n_default_char = AddName("Default__Character"),
             n_player_pkg = AddName("/Game/Characters/Player/Blueprints/BP_Player"),
             n_player_c = AddName("BP_Player_C"), n_other_pkg = AddName("/Game/Other"),
             n_other_c = AddName("BP_Other_C"), n_tick = AddName("ReceiveTick"),
             n_transient = AddName("/Engine/Transient"), n_title = AddName("UIM_Title_C"),
             n_mesh01 = AddName("Mesh_01"), n_mesh = AddName("Mesh");
    names::g_pool = reinterpret_cast<uintptr_t>(g_pool_mem.data());

    // Object array: two chunks of 64K slots.
    for (int c = 0; c < 2; ++c) g_chunk_ptrs[c] = reinterpret_cast<uintptr_t>(AlignedAlloc(65536 * 24));
    uintptr_t chunks = reinterpret_cast<uintptr_t>(g_chunk_ptrs.data());
    std::memcpy(g_array_hdr.data() + 0x10, &chunks, 8);
    int32_t hdr[4] = {2 * 65536, 65536 + 10, 2, 2};  // Max, Num, MaxChunks, NumChunks
    std::memcpy(g_array_hdr.data() + 0x20, hdr, sizeof(hdr));
    objects::g_chunked = reinterpret_cast<uintptr_t>(g_array_hdr.data()) + 0x10;
    objects::g_stride = 24;
    g_heap = static_cast<unsigned char*>(AlignedAlloc(0x10000));

    uintptr_t cls_class = NewObject(0, 0, N(n_class), 0);
    SetClass(cls_class, cls_class);
    uintptr_t cls_package = NewObject(2, cls_class, N(n_package), 0);
    uintptr_t pkg_core = NewObject(1, cls_package, N(n_core), 0);
    std::memcpy(reinterpret_cast<void*>(cls_class + 0x20), &pkg_core, 8);
    std::memcpy(reinterpret_cast<void*>(cls_package + 0x20), &pkg_core, 8);
    uintptr_t cls_function = NewObject(3, cls_class, N(n_function), pkg_core);
    uintptr_t cls_bpgc = NewObject(4, cls_class, N(n_bpgc), pkg_core);
    uintptr_t pkg_player = NewObject(5, cls_package, N(n_player_pkg), 0);
    uintptr_t player_c = NewObject(6, cls_bpgc, N(n_player_c), pkg_player);
    uintptr_t pkg_other = NewObject(7, cls_package, N(n_other_pkg), 0);
    uintptr_t other_c = NewObject(8, cls_bpgc, N(n_other_c), pkg_other);
    uintptr_t decoy_tick = NewObject(9, cls_function, N(n_tick), other_c);
    uintptr_t pkg_transient = NewObject(10, cls_package, N(n_transient), 0);
    uintptr_t cls_title = NewObject(11, cls_bpgc, N(n_title), pkg_transient);
    uintptr_t title = NewObject(12, cls_title, N(n_title, 2147482110), pkg_transient);
    uintptr_t title_other = NewObject(13, cls_title, N(n_title, 7), pkg_transient);
    uintptr_t mesh01 = NewObject(14, cls_package, N(n_mesh01), pkg_transient);
    uintptr_t mesh_0 = NewObject(15, cls_package, N(n_mesh, 1), pkg_transient);
    uintptr_t pkg_engine = NewObject(16, cls_package, N(n_engine), 0);
    uintptr_t cls_character = NewObject(17, cls_class, N(n_character), pkg_engine);
    uintptr_t default_char = NewObject(18, cls_character, N(n_default_char), pkg_engine);
    // An object in memory that is no longer readable, named like the tick.
    unsigned char* bad = static_cast<unsigned char*>(AlignedAlloc(0x10000));
    g_bad.insert(reinterpret_cast<uintptr_t>(bad));
    NewObject(20, cls_function, N(n_tick), player_c, bad);
    // The real tick, in the second chunk.
    uintptr_t tick = NewObject(65536 + 5, cls_function, N(n_tick), player_c);

    const char* kTick = "Function /Game/Characters/Player/Blueprints/BP_Player.BP_Player_C:ReceiveTick";
    EXPECT(GetFullName(tick) == kTick);
    EXPECT(FindObject(kTick) == tick);
    EXPECT(FindObject("Function /Game/Other.BP_Other_C:ReceiveTick") == decoy_tick);
    EXPECT(FindObject("UIM_Title_C /Engine/Transient.UIM_Title_C_2147482109") == title);
    EXPECT(FindObject("UIM_Title_C /Engine/Transient.UIM_Title_C_6") == title_other);
    EXPECT(FindObject("UIM_Title_C /Engine/Transient.UIM_Title_C_5") == 0);
    EXPECT(FindObject("BlueprintGeneratedClass /Engine/Transient.UIM_Title_C") == cls_title);
    EXPECT(FindObject("Package /Engine/Transient.Mesh_01") == mesh01);
    EXPECT(FindObject("Package /Engine/Transient.Mesh_0") == mesh_0);
    EXPECT(FindObject("Package /Engine/Transient.Mesh_1") == 0);
    EXPECT(FindObject("Character /Script/Engine.Default__Character") == default_char);
    EXPECT(FindObject("Class /Script/CoreUObject.Class") == cls_class);
    EXPECT(FindObject("Package /Game/Characters/Player/Blueprints/BP_Player") == pkg_player);
    EXPECT(FindObject("Function /Game/Characters/Player/Blueprints/BP_Player.BP_Player_C:NotLoaded") == 0);
    EXPECT(FindObject("NoSpaceHere") == 0);

    // Same answers as the old string scan, for every object's full name.
    // (Not the unreadable one: its name reads as "None None", which the old
    // scan matched against the unreadable object itself.)
    int compared = 0;
    objects::ForEach([&](int, UObject o) {
        if (o == reinterpret_cast<uintptr_t>(bad)) return;
        std::string full = GetFullName(o);
        UObject a = FindObject(full), b = OldFindObject(full);
        if (a != b) std::printf("  mismatch for %s: new %p old %p\n", full.c_str(), (void*)a, (void*)b);
        EXPECT(a == b);
        ++compared;
    });
    EXPECT(compared == 20);

    // ForEach visits every filled slot once, with its index, across chunks.
    std::set<int> seen;
    int visits = 0;
    objects::ForEach([&](int i, UObject o) {
        ++visits;
        seen.insert(i);
        EXPECT(o == objects::Get(i));
    });
    EXPECT(visits == 21 && seen.size() == 21 && seen.count(65536 + 5) == 1 && seen.count(19) == 0);

    // The region memo: one question per region, unreadable regions stay so.
    {
        mem::RegionMemo memo;
        int before = g_queries;
        uint64_t v;
        for (int i = 0; i < 1000; ++i) EXPECT(memo.Read(reinterpret_cast<uintptr_t>(g_heap) + (i % 100) * 8, v));
        EXPECT(g_queries - before == 1);
        EXPECT(!memo.Read(reinterpret_cast<uintptr_t>(bad) + 0x18, v));
        EXPECT(!memo.Read(reinterpret_cast<uintptr_t>(bad) + 0x40, v));
        EXPECT(g_queries - before == 2);
        // A read across two regions checks both.
        EXPECT(memo.IsReadable(reinterpret_cast<uintptr_t>(g_heap) + 0xFFF8, 16) ==
               mem::IsReadable(reinterpret_cast<uintptr_t>(g_heap) + 0xFFF8, 16));
        EXPECT(!memo.IsReadable(reinterpret_cast<uintptr_t>(bad) - 8, 16));
        EXPECT(!memo.IsReadable(0x100, 8));
    }

    // A name nobody has costs no object reads at all.
    {
        int before = g_queries;
        EXPECT(FindObject("Function /Game/Nowhere.Nothing_C:NeverRegistered") == 0);
        EXPECT(g_queries - before <= 2);  // the name pool's readability check
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all object scan tests passed\n");
    return 0;
}
