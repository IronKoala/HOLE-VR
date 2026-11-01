#include "ue/reflection.h"

#include "core/log.h"
#include "core/memory.h"

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

std::string FieldName(uintptr_t field) {
    return names::ToString(mem::ReadOr<FName>(field + o_ffield_name));
}
std::string FieldClassName(uintptr_t field) {
    uintptr_t fc = mem::ReadOr<uintptr_t>(field + o_ffield_class);
    return fc ? names::ToString(mem::ReadOr<FName>(fc)) : std::string();
}

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

std::vector<PropertyInfo> GetProperties(UObject s) {
    std::vector<PropertyInfo> out;
    if (!g_ready || !s) return out;
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
            if (mem::LooksLikePointer(t) && mem::IsReadable(t, 0x28)) p.inner_type = GetName(t);
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

}  // namespace holevr::ue::reflection
