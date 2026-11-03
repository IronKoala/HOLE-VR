#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

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
// cache the result (a game Blueprint class: keep it in an ObjectRef).
UObject FindObject(const std::string& full_name);

// True while obj is still the object the engine keeps at its own index slot;
// false once the engine destroyed it or garbage-collected it.
bool IsLive(UObject obj);

// A game object found by its full name and kept between calls, for Blueprint
// classes under /Game/. Those belong to a level's package: when you leave the
// level the engine frees the class, and a pointer kept from before points at
// freed memory. (Owner's 2026-11-03 Fatal Error "picking up a CD": the range
// timer class kept from the home level went to GetAllActorsOfClass in the next
// level, and the engine read the freed class.) Get() checks the kept pointer is
// still that object, by its object-array slot and its name, on every call, and
// finds it again, at most every retry_s seconds, when it is not. Native
// /Script/ classes and default objects never go away and need none of this.
class ObjectRef {
public:
    explicit ObjectRef(std::string full_name, double retry_s = 10.0)
        : full_name_(std::move(full_name)), retry_s_(retry_s) {}
    // The object, or 0 while it is not loaded.
    UObject Get();
    UObject Get(double now_s);  // with the caller's clock (tests)
    // Times the kept object went away with its level.
    int times_lost() const { return lost_; }

private:
    std::string full_name_;
    double retry_s_;
    UObject obj_ = 0;
    FName name_{};
    double next_find_ = -1e300;
    int lost_ = 0;
};

}  // namespace holevr::ue
