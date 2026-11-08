// Native test for the Blueprint bytecode decoder (no game, no Windows).
//   g++ -std=c++20 -Isrc -Itests/win_shim tests/test_script_dump.cpp -o /tmp/test_script_dump && /tmp/test_script_dump
//
// Builds bytecode the way the engine lays it out in memory (pointers inline,
// FScriptName as 12 bytes) and checks the listing reads back what was built,
// and that an unknown opcode stops the listing instead of guessing.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/memory.cpp"

// DumpClasses writes files; the test only checks Disassemble.
inline int CreateDirectoryW(const wchar_t*, void*) { return 0; }
inline FILE* _wfopen(const wchar_t*, const wchar_t*) { return nullptr; }
#include "ue/script_dump.cpp"

SIZE_T VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* mbi, SIZE_T) {
    uintptr_t base = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(0xFFFF);
    mbi->BaseAddress = reinterpret_cast<void*>(base);
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 0x10000;
    mbi->State = MEM_COMMIT;
    mbi->Protect = PAGE_READWRITE;
    return sizeof(*mbi);
}

namespace holevr::log {
void Info(const char*, ...) {}
void Warn(const char*, ...) {}
void Error(const char*, ...) {}
}  // namespace holevr::log

// ---- stand-ins for the engine lookups the decoder uses ----
static std::map<uintptr_t, std::string> g_obj_names, g_field_names;
static std::map<uintptr_t, uintptr_t> g_outer;
static std::map<uint32_t, std::string> g_names;
static std::vector<uint8_t> g_script;

namespace holevr::ue {
std::string GetName(UObject o) {
    auto it = g_obj_names.find(o);
    return it == g_obj_names.end() ? std::string() : it->second;
}
UObject GetOuter(UObject o) {
    auto it = g_outer.find(o);
    return it == g_outer.end() ? 0 : it->second;
}
UObject GetClass(UObject) { return 0; }
std::string GetPathName(UObject) { return {}; }
std::string GetFullName(UObject) { return {}; }
namespace objects {
void ForEach(const std::function<void(int, UObject)>&) {}
}  // namespace objects
namespace names {
std::string ToString(const FName& n) {
    auto it = g_names.find(n.comparison_index);
    std::string s = it == g_names.end() ? std::string() : it->second;
    if (!s.empty() && n.number > 0) s += "_" + std::to_string(n.number - 1);
    return s;
}
}  // namespace names
namespace reflection {
bool Ready() { return true; }
std::string FieldName(uintptr_t f) {
    auto it = g_field_names.find(f);
    return it == g_field_names.end() ? std::string() : it->second;
}
bool GetScript(UObject, uintptr_t* data, int32_t* size) {
    *data = reinterpret_cast<uintptr_t>(g_script.data());
    *size = static_cast<int32_t>(g_script.size());
    return true;
}
std::vector<PropertyInfo> GetProperties(UObject) { return {}; }
std::vector<UObject> GetFunctions(UObject) { return {}; }
UObject GetSuper(UObject) { return 0; }
}  // namespace reflection
}  // namespace holevr::ue

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

// Fake engine objects: 0x40-byte blocks so pointers are real, readable memory.
static std::vector<std::vector<uint8_t>> g_blocks;
static uintptr_t Fake(std::map<uintptr_t, std::string>& names, const std::string& name) {
    g_blocks.emplace_back(0x40, 0);
    uintptr_t p = reinterpret_cast<uintptr_t>(g_blocks.back().data());
    names[p] = name;
    return p;
}

static void B(uint8_t v) { g_script.push_back(v); }
template <typename T>
static void V(T v) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    g_script.insert(g_script.end(), b, b + sizeof(T));
}
static void Name(uint32_t idx, uint32_t number = 0) {
    V<uint32_t>(idx);
    V<uint32_t>(idx);
    V<uint32_t>(number);
}

static bool Has(const std::string& text, const std::string& want) {
    if (text.find(want) != std::string::npos) return true;
    std::printf("  missing: %s\n", want.c_str());
    return false;
}

int main() {
    g_blocks.reserve(64);
    const uintptr_t prop_a = Fake(g_field_names, "Count");
    const uintptr_t prop_b = Fake(g_field_names, "IsOpen");
    const uintptr_t prop_c = Fake(g_field_names, "WeaponManager");
    const uintptr_t prop_d = Fake(g_field_names, "Success");
    const uintptr_t prop_e = Fake(g_field_names, "Spec");
    const uintptr_t cls = Fake(g_obj_names, "BPC_WeaponManager_C");
    const uintptr_t fn = Fake(g_obj_names, "ChangeEquipWeapon");
    g_outer[fn] = cls;
    const uintptr_t st = Fake(g_obj_names, "Str_PartsAndPath");
    g_names[10] = "RefreshWeapon";
    g_names[11] = "SP_Sight";

    // 0x00: Count = 5
    B(0x0F); V(prop_a); B(0x00); V(prop_a); B(0x1D); V<int32_t>(5);
    // if not (IsOpen) goto 0x40
    B(0x07); V<uint32_t>(0x40); B(0x00); V(prop_b);
    // this.WeaponManager.BPC_WeaponManager_C::ChangeEquipWeapon(1, true)
    B(0x19); B(0x01); V(prop_c); V<uint32_t>(12); V<uintptr_t>(0); B(0x1C); V(fn); B(0x24); B(1); B(0x27); B(0x16);
    // RefreshWeapon()
    B(0x1B); Name(10); B(0x16);
    // out:Success = false
    B(0x14); B(0x48); V(prop_d); B(0x28);
    // Spec = Str_PartsAndPath{name:SP_Sight_2, 1.5}
    B(0x0F); V(prop_e); B(0x00); V(prop_e); B(0x2F); V(st); V<int32_t>(0x18); B(0x21); Name(11, 3); B(0x37);
    V<double>(1.5); B(0x30);
    // switch
    B(0x0F); V(prop_a); B(0x00); V(prop_a);
    B(0x69); V<uint16_t>(2); V<uint32_t>(0); B(0x00); V(prop_a);
    B(0x26); V<uint32_t>(0); B(0x1D); V<int32_t>(7);
    B(0x25); V<uint32_t>(0); B(0x1D); V<int32_t>(9);
    B(0x1D); V<int32_t>(-1);
    // vector + string
    B(0x0F); V(prop_e); B(0x00); V(prop_e); B(0x23); V<double>(1); V<double>(2); V<double>(3);
    B(0x0F); V(prop_e); B(0x00); V(prop_e); B(0x1F); for (char c : std::string("hello")) B(c); B(0);
    // return nop; end
    B(0x04); B(0x0B); B(0x53);

    std::string out = holevr::ue::script::Disassemble(1);
    std::printf("%s", out.c_str());
    EXPECT(Has(out, "0x0000: Count = 5"));
    EXPECT(Has(out, "if not (IsOpen) goto 0x0040"));
    EXPECT(Has(out, "this.WeaponManager.BPC_WeaponManager_C::ChangeEquipWeapon(1, true)"));
    EXPECT(Has(out, ": RefreshWeapon()"));
    EXPECT(Has(out, "out:Success = false"));
    EXPECT(Has(out, "Spec = Str_PartsAndPath{name:SP_Sight_2, 1.5}"));
    EXPECT(Has(out, "Count = switch(Count){1: 7; 0: 9; default: -1}"));
    EXPECT(Has(out, "Spec = vec(1, 2, 3)"));
    EXPECT(Has(out, "Spec = \"hello\""));
    EXPECT(Has(out, "return nop"));
    EXPECT(Has(out, ": end"));
    EXPECT(out.find("!! stopped") == std::string::npos);

    // An opcode the decoder doesn't know stops the listing and shows the bytes.
    g_script.clear();
    B(0x0F); V(prop_a); B(0x00); V(prop_a); B(0x1D); V<int32_t>(5);
    B(0x77); B(0x01); B(0x02); B(0x53);
    out = holevr::ue::script::Disassemble(1);
    std::printf("%s", out.c_str());
    EXPECT(Has(out, "Count = 5"));
    EXPECT(Has(out, "!! stopped at 0x0017: unknown opcode 0x77"));
    EXPECT(Has(out, " 77 01 02 53"));

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
