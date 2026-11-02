#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace holevr::mem {

struct Section {
    std::string name;
    uintptr_t start = 0;
    size_t size = 0;
};

struct Module {
    uintptr_t base = 0;
    size_t size = 0;
    std::vector<Section> sections;
    const Section* Find(const char* name) const;
    bool Contains(uintptr_t p) const { return p >= base && p < base + size; }
};

// The game's main executable module.
const Module& Game();

// True if [p, p+len) is committed, readable memory. Uses VirtualQuery with a
// small per-thread cache, so it is cheap to call in tight scan loops. We build
// with MinGW, which has no __try/__except, so every pointer we follow from
// game memory goes through this first.
bool IsReadable(uintptr_t p, size_t len);

template <typename T>
bool Read(uintptr_t p, T& out) {
    if (!IsReadable(p, sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(p), sizeof(T));
    return true;
}

template <typename T>
T ReadOr(uintptr_t p, T fallback = T{}) {
    T v;
    return Read(p, v) ? v : fallback;
}

// While one of these is alive, IsReadable on this thread remembers every
// memory region it looked up (normally only the last 4), and forgets them
// when the outermost one goes away. For short bursts of many reads spread
// over many heap blocks, such as walking a menu's 800 widgets, where the
// small cache keeps missing and each miss is a VirtualQuery system call.
// Keep its life short (one walk): memory freed while it is alive would still
// be taken as readable.
class ScopedRegionCache {
public:
    ScopedRegionCache();
    ~ScopedRegionCache();
    ScopedRegionCache(const ScopedRegionCache&) = delete;
    ScopedRegionCache& operator=(const ScopedRegionCache&) = delete;
    // VirtualQuery calls made on this thread since the outermost one began.
    static unsigned Lookups();
};

// True if [p, p+len) is committed read/write memory (heap objects, not code).
bool IsWritable(uintptr_t p, size_t len);

template <typename T>
bool Write(uintptr_t p, const T& value) {
    if (!IsWritable(p, sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(p), &value, sizeof(T));
    return true;
}

// Plausible user-mode heap/module pointer on x64 Windows.
inline bool LooksLikePointer(uintptr_t p) {
    return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 0x7) == 0;
}

// Byte pattern scan, IDA style: "48 8B 05 ?? ?? ?? ??".
uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern);

// Find the first occurrence of raw bytes inside a range.
uintptr_t FindBytes(uintptr_t start, size_t size, const void* bytes, size_t len);

}  // namespace holevr::mem
