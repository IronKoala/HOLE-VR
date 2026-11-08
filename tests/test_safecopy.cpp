// Windows test for mem::Read/Write on memory the game frees under us.
// Cross-compile and run (wine works too):
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -Isrc tests/test_safecopy.cpp src/core/memory.cpp
//       src/core/log.cpp -static -o test_safecopy.exe && wine test_safecopy.exe
//
// Reproduces the "Return to Title" crash of 2026-11-03: a pointer is read once
// (its memory region lands in this thread's IsReadable cache), the memory is
// then freed, and the same pointer is read again. Before SafeCopy the cache
// still said "readable" and the read crashed the game. Pages are decommitted
// (address kept reserved) so nothing else can be put there before the read.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/memory.h"

using namespace holevr;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

static void Decommit(uintptr_t p) { VirtualFree(reinterpret_cast<void*>(p), 4096, MEM_DECOMMIT); }

static uintptr_t NewPage() {
    void* p = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return reinterpret_cast<uintptr_t>(p);
}

// Child mode: a plain crash outside SafeCopy must still crash (the handler
// must not swallow the game's real crashes).
static int CrashChild() {
    uint64_t v = 0;
    mem::Read(NewPage(), v);  // installs the handler
    uintptr_t page = NewPage();
    volatile uint64_t* raw = reinterpret_cast<volatile uint64_t*>(page);
    std::printf("child: reading freed memory directly (should crash)\n");
    std::fflush(stdout);
    Decommit(page);
    uint64_t x = *raw;
    std::printf("child: survived the read (the handler swallowed a real crash)\n");
    return x == 12345 ? 41 : 42;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--crash") == 0) return CrashChild();

    // 1. Normal read/write still works, every size the copy special-cases.
    uintptr_t page = NewPage();
    EXPECT(page != 0);
    EXPECT(mem::Write<uint64_t>(page, 0x1122334455667788ull));
    EXPECT(mem::ReadOr<uint64_t>(page) == 0x1122334455667788ull);
    EXPECT(mem::ReadOr<uint32_t>(page) == 0x55667788u);
    EXPECT(mem::ReadOr<uint16_t>(page) == 0x7788u);
    EXPECT(mem::ReadOr<uint8_t>(page) == 0x88u);
    struct Big { char b[37]; } big{};
    for (int i = 0; i < 37; ++i) big.b[i] = static_cast<char>(i * 3);
    EXPECT(mem::Write(page + 100, big));
    Big back{};
    EXPECT(mem::Read(page + 100, back));
    EXPECT(std::memcmp(&back, &big, sizeof(big)) == 0);
    EXPECT(mem::CaughtFaults() == 0);

    // 2. The crash: the region is cached as readable, then released.
    EXPECT(mem::IsReadable(page + 0x10, 8));  // cached now
    Decommit(page);
    EXPECT(mem::IsReadable(page + 0x10, 8));  // the stale cache still says yes: the old bug
    uint64_t v = 0xdead;
    bool ok = mem::Read(page + 0x10, v);      // used to crash here
    EXPECT(!ok);
    EXPECT(v == 0xdead);
    EXPECT(mem::ReadOr<uint64_t>(page + 0x10, 7) == 7);
    EXPECT(mem::CaughtFaults() == 1);
    // The region was dropped from the cache: no second fault, just "unreadable".
    EXPECT(!mem::IsReadable(page + 0x10, 8));
    EXPECT(mem::ReadOr<uint64_t>(page + 0x10, 9) == 9);
    EXPECT(mem::CaughtFaults() == 1);

    // 3. Released (not just decommitted), read right away.
    uintptr_t page2 = NewPage();
    EXPECT(mem::ReadOr<uint32_t>(page2, 5) == 0);
    EXPECT(VirtualFree(reinterpret_cast<void*>(page2), 0, MEM_RELEASE));
    EXPECT(mem::ReadOr<uint32_t>(page2, 5) == 5);
    EXPECT(mem::CaughtFaults() == 2);

    // 4. A write into memory that turns read-only between check and write.
    uintptr_t page3 = NewPage();
    EXPECT(mem::Write<uint32_t>(page3, 1));
    DWORD old = 0;
    EXPECT(VirtualProtect(reinterpret_cast<void*>(page3), 4096, PAGE_READONLY, &old));
    EXPECT(!mem::SafeCopy(reinterpret_cast<void*>(page3), &old, 4));
    EXPECT(mem::ReadOr<uint32_t>(page3) == 1);

    // 5. Many faults in a row stay caught (another thread's stale cache, etc.).
    for (int i = 0; i < 50; ++i) {
        uintptr_t p = NewPage();
        EXPECT(mem::IsReadable(p, 8));
        Decommit(p);
        EXPECT(!mem::Read(p, v));
    }

    // 6. The same inside a ScopedRegionCache (menu widget walks), which
    // remembers every region for the length of one walk.
    {
        mem::ScopedRegionCache scope;
        uintptr_t p = NewPage();
        EXPECT(mem::ReadOr<uint64_t>(p, 3) == 0);
        Decommit(p);
        unsigned before = mem::CaughtFaults();
        EXPECT(mem::ReadOr<uint64_t>(p, 3) == 3);
        EXPECT(mem::CaughtFaults() == before + 1);
        EXPECT(mem::ReadOr<uint64_t>(p, 4) == 4);  // forgotten: no second fault
        EXPECT(mem::CaughtFaults() == before + 1);
    }

    // 7. RegionMemo (one pass over every object) keeps regions for the pass.
    {
        mem::RegionMemo memo;
        uintptr_t p = NewPage();
        uint64_t x = 1;
        EXPECT(memo.Read(p, x) && x == 0);
        Decommit(p);
        unsigned before = mem::CaughtFaults();
        EXPECT(!memo.Read(p, x));
        EXPECT(mem::CaughtFaults() == before + 1);
    }

    // 8. A real crash elsewhere is NOT swallowed: run ourselves with --crash.
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" --crash";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    EXPECT(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    // Windows: 0xc0000005. Wine hands the crash to its debugger and exits 0.
    std::printf("child exit code 0x%08lx (crashed as it should unless 41/42)\n", static_cast<unsigned long>(code));
    EXPECT(code != 41 && code != 42);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::printf("%s (%u faults caught)\n", failures ? "FAILED" : "all passed", mem::CaughtFaults());
    return failures ? 1 : 0;
}
