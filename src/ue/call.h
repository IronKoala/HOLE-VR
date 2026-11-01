#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ue/objects.h"

// Calling game functions by name with named parameters, laid out from the
// engine's own reflection data so nothing depends on hardcoded offsets.
//
//   ue::Call c(gun, "Gun_MagazineDrop");
//   c.Invoke();
//   bool ok = c.Get<bool>("Success");
//
// Only use functions whose parameters are plain data (numbers, bools,
// vectors, rotators, object pointers, FName). Strings, arrays and maps own
// heap memory and are not handled.
namespace holevr::ue {

// Function lookup up the class chain. Results are cached per class+name.
UObject FindFunction(UObject cls, const std::string& name);

// First live object (not a class default object) whose class is `cls` or a
// child of it. Linear scan, so cache the result.
UObject FindFirstInstance(UObject cls);
bool IsDefaultObject(UObject obj);

struct Vec3d {
    double x = 0, y = 0, z = 0;
};
struct Rot3d {
    double pitch = 0, yaw = 0, roll = 0;
};

class Call {
public:
    Call(UObject obj, const char* function_name);
    Call(UObject obj, UObject function);

    bool Valid() const { return obj_ && fn_; }
    UObject Function() const { return fn_; }

    template <typename T>
    Call& Set(const char* param, const T& value) {
        int32_t o = Offset(param, sizeof(T));
        if (o >= 0) std::memcpy(buf_.data() + o, &value, sizeof(T));
        return *this;
    }
    template <typename T>
    T Get(const char* param, T fallback = T{}) const {
        int32_t o = Offset(param, sizeof(T));
        if (o < 0) return fallback;
        T v;
        std::memcpy(&v, buf_.data() + o, sizeof(T));
        return v;
    }
    // Return value of functions that have one ("ReturnValue").
    template <typename T>
    T Result(T fallback = T{}) const {
        return Get<T>("ReturnValue", fallback);
    }

    // Runs the function on the game thread's caller. Returns false if the
    // object or function could not be found.
    bool Invoke();

private:
    int32_t Offset(const char* param, size_t size) const;

    UObject obj_ = 0;
    UObject fn_ = 0;
    std::string name_;
    std::vector<uint8_t> buf_;
};

// Property access by name on any object, searched up the class chain and
// cached. Returns false if the property does not exist or the read fails.
int32_t PropertyOffset(UObject obj, const std::string& name);
template <typename T>
bool GetProp(UObject obj, const std::string& name, T& out);
template <typename T>
bool SetProp(UObject obj, const std::string& name, const T& value);

}  // namespace holevr::ue

#include "core/memory.h"

namespace holevr::ue {
template <typename T>
bool GetProp(UObject obj, const std::string& name, T& out) {
    int32_t o = PropertyOffset(obj, name);
    return o >= 0 && mem::Read(obj + o, out);
}
template <typename T>
bool SetProp(UObject obj, const std::string& name, const T& value) {
    int32_t o = PropertyOffset(obj, name);
    if (o < 0 || !mem::IsReadable(obj + o, sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(obj + o), &value, sizeof(T));
    return true;
}
}  // namespace holevr::ue
