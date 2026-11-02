#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "ue/names.h"

namespace holevr::ue {

// UObjectBase layout, UE 5.x 64-bit (unchanged since UE 4.11):
//   0x00 vtable, 0x08 EObjectFlags, 0x0C InternalIndex,
//   0x10 UClass* ClassPrivate, 0x18 FName NamePrivate, 0x20 UObject* OuterPrivate
namespace off {
constexpr uintptr_t kObjFlags = 0x08;
constexpr uintptr_t kObjIndex = 0x0C;
constexpr uintptr_t kObjClass = 0x10;
constexpr uintptr_t kObjName = 0x18;
constexpr uintptr_t kObjOuter = 0x20;
}  // namespace off

using UObject = uintptr_t;  // Raw address; all access goes through checked reads.

namespace objects {
// Locates GUObjectArray by scanning the exe's data for the chunked object
// array header and checking that item N really holds the object whose
// InternalIndex is N. Detects the FUObjectItem size at the same time.
bool Init();
bool Ready();
uintptr_t ArrayAddress();
int Count();
UObject Get(int index);
void ForEach(const std::function<void(int index, UObject obj)>& fn);
}  // namespace objects

UObject GetClass(UObject obj);
UObject GetOuter(UObject obj);
FName GetFName(UObject obj);
std::string GetName(UObject obj);
// "Outer.Outer.Name", the engine's path name without the class.
std::string GetPathName(UObject obj);
// "ClassName /Package/Path.Object", what the engine's GetFullName returns.
std::string GetFullName(UObject obj);

// Exact full-name lookup, e.g. "Class /Script/Engine.Actor". Linear scan, so
// cache the result.
UObject FindObject(const std::string& full_name);

}  // namespace holevr::ue
