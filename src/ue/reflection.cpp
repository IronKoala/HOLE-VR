#include "ue/reflection.h"

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"

namespace holevr::ue::reflection {
namespace {

// UStruct (after UField, which is UObjectBase + Next):
//   SuperStruct, Children, ChildProperties, PropertiesSize at consecutive
//   slots; where they start depends on build options, so we find it.
uintptr_t o_super = 0;
uintptr_t o_children = 0;
uintptr_t o_child_props = 0;
uintptr_t o_props_size = 0;
constexpr uintptr_t o_field_next = 0x28;  // UField::Next

// FField: 0x00 vtable, 0x08 FFieldClass*, then Owner (FFieldVariant), Next, Name.
uintptr_t o_ffield_class = 0x08;
uintptr_t o_ffield_next = 0;
uintptr_t o_ffield_name = 0;
// FProperty members, relative to the FField name slot once that is known.
uintptr_t o_array_dim = 0, o_element_size = 0, o_prop_flags = 0, o_offset = 0, o_prop_size = 0;

bool g_ready = false;

bool FindSuperOffset(UObject actor, UObject object) {
    for (uintptr_t o = 0x30; o <= 0x60; o += 8) {
        if (mem::ReadOr<uintptr_t>(actor + o) == object) {
            o_super = o;
            o_children = o + 0x08;
            o_child_props = o + 0x10;
            o_props_size = o + 0x18;
            return true;
        }
    }
    return false;
}

// Walk Actor's own properties with a guessed (next, name) layout and look for
// members Actor has had in every UE5 version.
bool FFieldLayoutWorks(UObject actor, uintptr_t next, uintptr_t name) {
    uintptr_t f = mem::ReadOr<uintptr_t>(actor + o_child_props);
    bool tick = false, root = false;
    for (int n = 0; f && n < 512; ++n) {
        std::string s = names::ToString(mem::ReadOr<FName>(f + name));
        if (s.empty()) return false;
        if (s == "PrimaryActorTick") tick = true;
        if (s == "RootComponent") root = true;
        f = mem::ReadOr<uintptr_t>(f + next);
    }
    return tick && root;
}

bool OffsetsFit(UObject ustruct) {
    int32_t size = mem::ReadOr<int32_t>(ustruct + o_props_size);
    uintptr_t f = mem::ReadOr<uintptr_t>(ustruct + o_child_props);
    int count = 0;
    for (; f && count < 512; ++count) {
        int32_t off = mem::ReadOr<int32_t>(f + o_offset, -1);
        int32_t esz = mem::ReadOr<int32_t>(f + o_element_size, -1);
        if (off < 0x28 || off >= size || esz <= 0 || esz > size) return false;
        f = mem::ReadOr<uintptr_t>(f + o_ffield_next);
    }
    return count > 0;
}

}  // namespace

std::string FieldName(uintptr_t field) {
    if (!o_ffield_name || !field) return {};
    return names::ToString(mem::ReadOr<FName>(field + o_ffield_name));
}
std::string FieldClassName(uintptr_t field) {
    if (!field) return {};
    uintptr_t fc = mem::ReadOr<uintptr_t>(field + o_ffield_class);
    return fc ? names::ToString(mem::ReadOr<FName>(fc)) : std::string();
}

bool GetScript(UObject s, uintptr_t* data, int32_t* size) {
    if (!g_ready || !s) return false;
    // TArray<uint8> Script { Data, Num, Max } after PropertiesSize + MinAlignment.
    const uintptr_t o = o_props_size + 0x08;
    uintptr_t d = 0;
    int32_t num = 0, max = 0;
    if (!mem::Read(s + o, d) || !mem::Read(s + o + 8, num) || !mem::Read(s + o + 12, max)) return false;
    if (!d || num <= 0 || num > max || num > (64 << 20) || !mem::IsReadable(d, num)) return false;
    if (mem::ReadOr<uint8_t>(d + num - 1, 0) != 0x53) return false;  // EX_EndOfScript
    *data = d;
    *size = num;
    return true;
}

bool Init() {
    if (g_ready) return true;
    UObject object = FindObject("Class /Script/CoreUObject.Object");
    UObject actor = FindObject("Class /Script/Engine.Actor");
    if (!object || !actor) return false;  // Engine still starting; caller retries.
    if (!FindSuperOffset(actor, object)) return false;
    const uintptr_t layouts[][2] = {{0x20, 0x28}, {0x18, 0x20}, {0x28, 0x30}};
    for (auto& l : layouts) {
        if (FFieldLayoutWorks(actor, l[0], l[1])) {
            o_ffield_next = l[0];
            o_ffield_name = l[1];
            break;
        }
    }
    // Actor's properties can still be registering on the first few tries;
    // the caller retries and reports a timeout if this never succeeds.
    if (!o_ffield_name) return false;
    // FField ends with Name (8) + FlagsPrivate (8). FProperty then has
    // ArrayDim, ElementSize, PropertyFlags (8), RepIndex (2), RepCondition (1),
    // pad, Offset_Internal. Try the standard layout and a 4-byte-shifted one.
    uintptr_t base = o_ffield_name + 0x10;
    for (uintptr_t shift : {0u, 8u, 4u}) {
        o_array_dim = base + shift;
        o_element_size = base + shift + 0x04;
        o_prop_flags = base + shift + 0x08;
        o_offset = base + shift + 0x14;
        // RepNotifyFunc (FName) + four link pointers follow Offset_Internal.
        o_prop_size = o_offset + 0x04 + 0x08 + 0x20;
        if (OffsetsFit(actor)) {
            g_ready = true;
            break;
        }
    }
    if (!g_ready) return false;
    log::Info("reflection: SuperStruct=0x%llx ChildProperties=0x%llx FField.Next=0x%llx "
              "FField.Name=0x%llx FProperty.Offset=0x%llx sizeof(FProperty)=0x%llx",
              (unsigned long long)o_super, (unsigned long long)o_child_props,
              (unsigned long long)o_ffield_next, (unsigned long long)o_ffield_name,
              (unsigned long long)o_offset, (unsigned long long)o_prop_size);
    return true;
}

bool Ready() { return g_ready; }

UObject GetSuper(UObject s) { return g_ready && s ? mem::ReadOr<uintptr_t>(s + o_super) : 0; }
int32_t GetPropertiesSize(UObject s) { return g_ready && s ? mem::ReadOr<int32_t>(s + o_props_size) : 0; }

// Reading every variable of a class decodes each name through guarded reads
// (about 1 ms for a weapon Blueprint and its parents). Counted so the perf
// report shows any code that does it every frame instead of once
// (2026-11-06: three uncached pistol lookups cost 9 ms of every frame).
static perf::Counter g_perf_props{"lookups: all of a class's variables read"};

std::vector<PropertyInfo> GetProperties(UObject s) {
    std::vector<PropertyInfo> out;
    if (!g_ready || !s) return out;
    static const bool registered = [] {
        g_perf_props.per_second_total = true;
        perf::Register(&g_perf_props);
        return true;
    }();
    (void)registered;
    perf::Scope timed(g_perf_props);
    uintptr_t f = mem::ReadOr<uintptr_t>(s + o_child_props);
    for (int n = 0; f && n < 4096; ++n) {
        PropertyInfo p;
        p.name = FieldName(f);
        p.type = FieldClassName(f);
        p.offset = mem::ReadOr<int32_t>(f + o_offset);
        p.element_size = mem::ReadOr<int32_t>(f + o_element_size);
        p.array_dim = mem::ReadOr<int32_t>(f + o_array_dim, 1);
        p.flags = mem::ReadOr<uint64_t>(f + o_prop_flags);
        // Object/class/struct/enum-backed properties keep their type pointer
        // as the first member after FProperty.
        if (p.type == "ObjectProperty" || p.type == "ClassProperty" ||
            p.type == "SoftObjectProperty" || p.type == "SoftClassProperty" ||
            p.type == "WeakObjectProperty" || p.type == "LazyObjectProperty" ||
            p.type == "InterfaceProperty" || p.type == "StructProperty") {
            uintptr_t t = mem::ReadOr<uintptr_t>(f + o_prop_size);
            if (mem::LooksLikePointer(t) && mem::IsReadable(t, 0x28)) {
                p.inner = t;
                p.inner_type = GetName(t);
            }
        }
        if (p.type == "BoolProperty") {
            // FBoolProperty: FieldSize, ByteOffset, ByteMask, FieldMask. The
            // value lives in the byte at Offset + ByteOffset under ByteMask
            // (0xFF for a plain bool, one bit for a bitfield).
            p.offset += mem::ReadOr<uint8_t>(f + o_prop_size + 1);
            p.bool_mask = mem::ReadOr<uint8_t>(f + o_prop_size + 2);
        }
        out.push_back(std::move(p));
        f = mem::ReadOr<uintptr_t>(f + o_ffield_next);
    }
    return out;
}

std::vector<UObject> GetFunctions(UObject s) {
    std::vector<UObject> out;
    if (!g_ready || !s) return out;
    UObject f = mem::ReadOr<uintptr_t>(s + o_children);
    for (int n = 0; f && n < 4096; ++n) {
        out.push_back(f);
        f = mem::ReadOr<uintptr_t>(f + o_field_next);
    }
    return out;
}

bool IsChildOf(UObject s, UObject base) {
    for (int n = 0; s && n < 128; ++n, s = GetSuper(s))
        if (s == base) return true;
    return false;
}

bool IsA(UObject obj, UObject base_class) { return IsChildOf(GetClass(obj), base_class); }

int32_t FindPropertyOffset(UObject s, const std::string& name) {
    for (int n = 0; s && n < 128; ++n, s = GetSuper(s)) {
        for (const auto& p : GetProperties(s))
            if (p.name == name) return p.offset;
    }
    return -1;
}

namespace {
PropertyInfo FindPropertyUncached(UObject s, const std::string& name) {
    for (int n = 0; s && n < 128; ++n, s = GetSuper(s)) {
        for (auto& p : GetProperties(s))
            if (p.name == name) return p;
    }
    return {};
}

// FindProperty walks the whole class chain and decodes every property's
// names through guarded reads: about 3 ms for a weapon Blueprint, and the
// holster code asked for the pistol's IsOffset three times a frame (owner
// 2026-11-06: tick 3 took 8 ms of every 14 ms frame). A class's properties
// never change once it is loaded, so the answer is kept, checked against
// the struct's own index and name each time so a freed class whose memory
// now holds another object is looked up again. A miss is kept for 2 s only:
// a class still linking may gain the property.
struct CachedProperty {
    int32_t index = -1;
    FName name{};
    bool found = false;
    std::chrono::steady_clock::time_point miss_until{};
    PropertyInfo info;
};
std::mutex g_find_mutex;
std::map<std::pair<UObject, std::string>, CachedProperty> g_find_cache;

bool SameStruct(UObject s, const CachedProperty& c) {
    if (mem::ReadOr<int32_t>(s + off::kObjIndex, -2) != c.index) return false;
    const FName now = mem::ReadOr<FName>(s + off::kObjName);
    return std::memcmp(&now, &c.name, sizeof(FName)) == 0;
}
}  // namespace

PropertyInfo FindProperty(UObject s, const std::string& name) {
    if (!s || !g_ready) return {};
    const auto now = std::chrono::steady_clock::now();
    auto key = std::make_pair(s, name);
    {
        std::lock_guard<std::mutex> lock(g_find_mutex);
        auto it = g_find_cache.find(key);
        if (it != g_find_cache.end() && SameStruct(s, it->second) &&
            (it->second.found || now < it->second.miss_until))
            return it->second.info;
    }
    PropertyInfo info = FindPropertyUncached(s, name);
    CachedProperty c;
    c.index = mem::ReadOr<int32_t>(s + off::kObjIndex, -2);
    c.name = mem::ReadOr<FName>(s + off::kObjName);
    c.found = info.valid();
    c.miss_until = now + std::chrono::seconds(2);
    c.info = info;
    std::lock_guard<std::mutex> lock(g_find_mutex);
    if (g_find_cache.size() >= 8192) g_find_cache.clear();
    g_find_cache[std::move(key)] = std::move(c);
    return info;
}

}  // namespace holevr::ue::reflection
