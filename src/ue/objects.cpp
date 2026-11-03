#include "ue/objects.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "core/memory.h"

namespace holevr::ue {
namespace objects {
namespace {

// FChunkedFixedUObjectArray, at offset 0x10 inside FUObjectArray:
//   0x00 FUObjectItem** Objects
//   0x08 FUObjectItem*  PreAllocatedObjects
//   0x10 int32 MaxElements, 0x14 NumElements, 0x18 MaxChunks, 0x1C NumChunks
constexpr int kChunkSize = 64 * 1024;

uintptr_t g_chunked = 0;  // address of FChunkedFixedUObjectArray
size_t g_stride = 0;      // sizeof(FUObjectItem)

uintptr_t ItemObject(uintptr_t chunks, size_t stride, int index) {
    uintptr_t chunk = mem::ReadOr<uintptr_t>(chunks + (index / kChunkSize) * 8ull);
    if (!chunk) return 0;
    return mem::ReadOr<uintptr_t>(chunk + (index % kChunkSize) * stride);
}

// The first objects the engine creates all have InternalIndex == slot.
bool StrideMatches(uintptr_t chunks, size_t stride) {
    int checked = 0;
    for (int i = 0; i < 64; ++i) {
        uintptr_t obj = ItemObject(chunks, stride, i);
        if (!obj) continue;
        if (!mem::LooksLikePointer(obj)) return false;
        int32_t idx;
        if (!mem::Read(obj + off::kObjIndex, idx) || idx != i) return false;
        ++checked;
    }
    return checked >= 32;
}

bool TryCandidate(uintptr_t p) {
    int32_t ints[4];
    if (!mem::Read(p + 0x10, ints)) return false;
    int32_t max_elements = ints[0], num_elements = ints[1], max_chunks = ints[2], num_chunks = ints[3];
    if (max_elements < kChunkSize || max_elements > 64 * 1024 * 1024) return false;
    if (num_elements < 1000 || num_elements > max_elements) return false;
    if (num_chunks < 1 || num_chunks > max_chunks) return false;
    if (num_chunks != (num_elements + kChunkSize - 1) / kChunkSize) return false;
    if (max_chunks != (max_elements + kChunkSize - 1) / kChunkSize) return false;
    uintptr_t chunks = mem::ReadOr<uintptr_t>(p);
    if (!mem::LooksLikePointer(chunks)) return false;
    for (size_t stride : {24u, 16u, 32u}) {
        if (StrideMatches(chunks, stride)) {
            g_chunked = p;
            g_stride = stride;
            return true;
        }
    }
    return false;
}

}  // namespace

bool Init() {
    if (g_chunked) return true;
    for (const auto& s : mem::Game().sections) {
        if (s.name != ".data") continue;
        uintptr_t end = s.start + s.size - 0x20;
        for (uintptr_t p = (s.start + 7) & ~uintptr_t(7); p < end; p += 8) {
            if (TryCandidate(p)) {
                log::Info("GUObjectArray found at exe+0x%llx, %d objects, item size %zu",
                          static_cast<unsigned long long>(p - 0x10 - mem::Game().base), Count(),
                          g_stride);
                return true;
            }
        }
    }
    return false;
}

bool Ready() { return g_chunked != 0; }
uintptr_t ArrayAddress() { return g_chunked ? g_chunked - 0x10 : 0; }
int Count() { return g_chunked ? mem::ReadOr<int32_t>(g_chunked + 0x14) : 0; }

UObject Get(int index) {
    if (!g_chunked || index < 0 || index >= Count()) return 0;
    uintptr_t chunks = mem::ReadOr<uintptr_t>(g_chunked);
    return ItemObject(chunks, g_stride, index);
}

void ForEach(const std::function<void(int, UObject)>& fn) {
    if (!g_chunked) return;
    int n = Count();
    uintptr_t chunks = mem::ReadOr<uintptr_t>(g_chunked);
    if (!chunks) return;
    // The engine allocates each chunk of object slots once and never frees
    // it, so one readability check per chunk covers all its slots (instead
    // of three checked reads per object).
    for (int first = 0; first < n; first += kChunkSize) {
        uintptr_t chunk = mem::ReadOr<uintptr_t>(chunks + (first / kChunkSize) * 8ull);
        if (!chunk) continue;
        int count = std::min(kChunkSize, n - first);
        bool direct = mem::IsReadable(chunk, static_cast<size_t>(count) * g_stride);
        for (int k = 0; k < count; ++k) {
            uintptr_t item = chunk + static_cast<size_t>(k) * g_stride;
            UObject o = direct ? *reinterpret_cast<const volatile uintptr_t*>(item)
                               : mem::ReadOr<uintptr_t>(item);
            if (o) fn(first + k, o);
        }
    }
}

}  // namespace objects

UObject GetClass(UObject obj) { return obj ? mem::ReadOr<uintptr_t>(obj + off::kObjClass) : 0; }
UObject GetOuter(UObject obj) { return obj ? mem::ReadOr<uintptr_t>(obj + off::kObjOuter) : 0; }
FName GetFName(UObject obj) { return obj ? mem::ReadOr<FName>(obj + off::kObjName) : FName{}; }
std::string GetName(UObject obj) { return obj ? names::ToString(GetFName(obj)) : std::string("None"); }

std::string GetPathName(UObject obj) {
    if (!obj) return "None";
    std::vector<UObject> chain;
    for (UObject o = obj; o && chain.size() < 64; o = GetOuter(o)) chain.push_back(o);
    // chain is innermost-first; walk it package-first. The engine writes ':'
    // before a subobject of a package's top-level asset (depth 2), '.' elsewhere.
    std::string out;
    size_t depth = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it, ++depth) {
        if (depth > 0) out += depth == 2 ? ":" : ".";
        out += GetName(*it);
    }
    return out;
}

std::string GetFullName(UObject obj) {
    if (!obj) return "None";
    return GetName(GetClass(obj)) + " " + GetPathName(obj);
}

UObject FindObject(const std::string& full_name) {
    size_t space = full_name.find(' ');
    if (space == std::string::npos) return 0;
    // Compare the object's own name first: far cheaper than building every path.
    size_t last = full_name.find_last_of(".:");
    std::string short_name = full_name.substr(last == std::string::npos ? space + 1 : last + 1);
    // An object's name is an entry in the name pool plus a number ("Bullet_3"
    // is entry "Bullet", number 4; "Bullet" is number 0). Look the entry up
    // once, then compare 8 bytes per object instead of building each name.
    // A name that is not in the pool belongs to no object.
    FName wanted[2];
    int wanted_n = 0;
    if (names::Find(short_name, &wanted[wanted_n])) ++wanted_n;
    size_t us = short_name.rfind('_');
    if (us != std::string::npos && us > 0 && us + 1 < short_name.size()) {
        const std::string digits = short_name.substr(us + 1);
        bool canonical = digits.size() <= 10 && (digits.size() == 1 || digits[0] != '0') &&
                         digits.find_first_not_of("0123456789") == std::string::npos;
        unsigned long long number = canonical ? std::strtoull(digits.c_str(), nullptr, 10) : 0;
        if (canonical && number < 0xFFFFFFFFull &&
            names::Find(short_name.substr(0, us), &wanted[wanted_n])) {
            wanted[wanted_n].number = static_cast<uint32_t>(number + 1);
            ++wanted_n;
        }
    }
    if (wanted_n == 0) return 0;
    mem::RegionMemo memo;
    UObject found = 0;
    objects::ForEach([&](int, UObject o) {
        if (found) return;
        FName name;
        if (!memo.Read(o + off::kObjName, name)) return;
        bool match = false;
        for (int i = 0; i < wanted_n; ++i)
            match |= name.comparison_index == wanted[i].comparison_index && name.number == wanted[i].number;
        if (match && GetFullName(o) == full_name) found = o;
    });
    return found;
}

}  // namespace holevr::ue
