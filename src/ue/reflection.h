#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ue/objects.h"

namespace holevr::ue::reflection {

// Works out UStruct/FField/FProperty member offsets at runtime by checking
// them against classes every UE5 game has (Object, Actor). Offsets that move
// between engine versions are never hardcoded without a check.
bool Init();
bool Ready();

struct PropertyInfo {
    std::string name;
    std::string type;        // FFieldClass name, e.g. "FloatProperty"
    std::string inner_type;  // struct/class/object type when there is one
    int32_t offset = 0;
    int32_t element_size = 0;
    int32_t array_dim = 1;
    uint64_t flags = 0;      // EPropertyFlags
    UObject inner = 0;       // the UStruct/UClass behind inner_type
    // BoolProperty only: bitfield bools share a byte, so writes must use the
    // mask. offset already includes the byte offset.
    uint8_t bool_mask = 0;
    bool valid() const { return !name.empty(); }
};

UObject GetSuper(UObject ustruct);
int32_t GetPropertiesSize(UObject ustruct);
// Own properties only, not inherited ones.
std::vector<PropertyInfo> GetProperties(UObject ustruct);
// Own functions (UFunction objects in the Children list).
std::vector<UObject> GetFunctions(UObject ustruct);
bool IsChildOf(UObject ustruct, UObject base);
// Is obj an instance of base (a UClass)?
bool IsA(UObject obj, UObject base_class);
// Offset of a named property, searching up the inheritance chain. -1 if none.
int32_t FindPropertyOffset(UObject ustruct, const std::string& name);
// Same search, returning the full property. valid() is false if not found.
PropertyInfo FindProperty(UObject ustruct, const std::string& name);

// EPropertyFlags bits we print in dumps.
constexpr uint64_t CPF_Parm = 0x80;
constexpr uint64_t CPF_OutParm = 0x100;
constexpr uint64_t CPF_ReturnParm = 0x400;

}  // namespace holevr::ue::reflection
