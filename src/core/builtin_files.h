#pragma once

#include <string>

// Files built into the DLL from the repo's defaults/ folder: the playtester's
// hand poses (grips), holster spots and magazine seats. A fresh install has
// none of them, so they are written into the hole-vr folder on start-up
// wherever the player has no file of their own.
namespace holevr::builtin_files {

struct File {
    const wchar_t* path;  // relative to the hole-vr folder, e.g. L"grips\\BP_HG_C.grip.ini"
    const unsigned char* data;
    unsigned size;
};

// Generated at build time (cmake/embed_files.cmake).
extern const File kFiles[];
extern const int kFileCount;

struct SeedResult {
    int written = 0;  // missing, now written
    int kept = 0;     // the player already had it: left alone
    int failed = 0;   // could not be written
};

// Writes every built-in file missing from mod_dir. Never overwrites a file
// that exists. Safe to call from DllMain (kernel32 file calls only).
SeedResult SeedMissing(const std::wstring& mod_dir);

}  // namespace holevr::builtin_files
