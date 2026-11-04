// Native tests for the built-in files (defaults/ -> DLL -> player's folder).
//   cmake -DSRC_DIR=defaults -DOUT=/tmp/builtin_files_data.cpp -P cmake/embed_files.cmake
//   g++ -std=c++20 -Isrc -Itests/win_shim_files tests/test_builtin_files.cpp src/core/builtin_files.cpp
//       /tmp/builtin_files_data.cpp -o /tmp/test_builtin_files && /tmp/test_builtin_files defaults
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <windows.h>

#include "core/builtin_files.h"

using holevr::builtin_files::kFileCount;
using holevr::builtin_files::kFiles;
using holevr::builtin_files::SeedMissing;

static int failures = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
            ++failures;                                               \
        }                                                             \
    } while (0)

// ---- a fake folder for the Win32 file calls ----
static std::set<std::wstring> g_dirs;
static std::map<std::wstring, std::string> g_files;
static std::map<intptr_t, std::wstring> g_open;
static DWORD g_error = 0;
static intptr_t g_next = 100;
static std::wstring g_fail_write;  // a write to this path fails
static std::wstring g_last_dir_needed;

static std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L'\\')); }

BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES*) {
    if (!g_dirs.count(Parent(path))) return 0;  // like Windows: the parent must exist
    g_dirs.insert(path);
    return 1;
}
HANDLE CreateFileW(LPCWSTR path, DWORD, DWORD, SECURITY_ATTRIBUTES*, DWORD disposition, DWORD, HANDLE) {
    EXPECT(disposition == CREATE_NEW);
    if (!g_dirs.count(Parent(path))) {
        g_error = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    if (g_files.count(path)) {
        g_error = ERROR_FILE_EXISTS;
        return INVALID_HANDLE_VALUE;
    }
    g_files[path] = "";
    g_open[g_next] = path;
    return reinterpret_cast<HANDLE>(g_next++);
}
BOOL WriteFile(HANDLE h, const void* data, DWORD size, LPDWORD written, void*) {
    const std::wstring& path = g_open.at(reinterpret_cast<intptr_t>(h));
    if (path == g_fail_write) {
        g_files[path] = "par";  // half written
        *written = 3;
        return 0;
    }
    g_files[path].append(static_cast<const char*>(data), size);
    *written = size;
    return 1;
}
BOOL CloseHandle(HANDLE h) { return g_open.erase(reinterpret_cast<intptr_t>(h)) == 1; }
BOOL DeleteFileW(LPCWSTR path) { return g_files.erase(path) == 1; }
DWORD GetLastError() { return g_error; }

static void Reset() {
    g_dirs = {L"C:", L"C:\\Win64", L"C:\\Win64\\hole-vr"};
    g_files.clear();
    g_open.clear();
    g_fail_write.clear();
}

static std::string Bytes(const holevr::builtin_files::File& f) {
    return std::string(reinterpret_cast<const char*>(f.data), f.size);
}
static std::wstring Full(const holevr::builtin_files::File& f) { return std::wstring(L"C:\\Win64\\hole-vr\\") + f.path; }

int main(int argc, char** argv) {
    const std::wstring dir = L"C:\\Win64\\hole-vr";

    // The embedded bytes are exactly the files in defaults/.
    if (argc > 1) {
        namespace fs = std::filesystem;
        int on_disk = 0;
        for (const auto& e : fs::recursive_directory_iterator(argv[1])) {
            if (!e.is_regular_file() || e.path().filename() == "README.md") continue;
            ++on_disk;
        }
        EXPECT(on_disk == kFileCount);
        for (int i = 0; i < kFileCount; ++i) {
            std::wstring rel = kFiles[i].path;
            for (wchar_t& c : rel)
                if (c == L'\\') c = L'/';
            std::ifstream in(fs::path(argv[1]) / fs::path(rel), std::ios::binary);
            EXPECT(in.good());
            std::stringstream ss;
            ss << in.rdbuf();
            EXPECT(ss.str() == Bytes(kFiles[i]));
        }
        std::printf("%d built-in files match defaults/\n", kFileCount);
    }
    EXPECT(kFileCount > 0);
    bool have_grip = false, have_holsters = false;
    for (int i = 0; i < kFileCount; ++i) {
        const std::wstring p = kFiles[i].path;
        have_grip |= p == L"grips\\BP_HG_C.grip.ini";
        have_holsters |= p == L"holsters.ini";
        EXPECT(p.find(L"body-measure") == std::wstring::npos);  // each player's own
        EXPECT(p != L"hole-vr.ini");
    }
    EXPECT(have_grip);
    EXPECT(have_holsters);

    // Fresh install: everything written, the grips folder made first.
    Reset();
    auto r = SeedMissing(dir);
    EXPECT(r.written == kFileCount);
    EXPECT(r.kept == 0 && r.failed == 0);
    EXPECT(g_dirs.count(L"C:\\Win64\\hole-vr\\grips"));
    for (int i = 0; i < kFileCount; ++i) EXPECT(g_files[Full(kFiles[i])] == Bytes(kFiles[i]));
    EXPECT(g_open.empty());

    // Next start, after the player saved their own pistol grip: theirs stays.
    const std::wstring grip = L"C:\\Win64\\hole-vr\\grips\\BP_HG_C.grip.ini";
    g_files[grip] = "name=BP_HG_C.grip\nhand=right\n; the player's own\n";
    r = SeedMissing(dir);
    EXPECT(r.written == 0);
    EXPECT(r.kept == kFileCount);
    EXPECT(g_files[grip] == "name=BP_HG_C.grip\nhand=right\n; the player's own\n");

    // A deleted file comes back; the rest are left alone.
    g_files.erase(L"C:\\Win64\\hole-vr\\holsters.ini");
    r = SeedMissing(dir);
    EXPECT(r.written == 1 && r.kept == kFileCount - 1);

    // A write that fails leaves no half file behind.
    Reset();
    g_fail_write = grip;
    r = SeedMissing(dir);
    EXPECT(r.failed == 1 && r.written == kFileCount - 1);
    EXPECT(!g_files.count(grip));
    EXPECT(g_open.empty());

    // No hole-vr folder (the game folder is read-only): nothing written, all failed.
    Reset();
    g_dirs = {L"C:"};
    r = SeedMissing(dir);
    EXPECT(r.written == 0 && r.failed == kFileCount);

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all built-in file tests passed\n");
    return 0;
}
