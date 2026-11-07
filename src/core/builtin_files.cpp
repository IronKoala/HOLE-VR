#include "core/builtin_files.h"

#include <windows.h>

namespace holevr::builtin_files {

SeedResult SeedMissing(const std::wstring& mod_dir) {
    SeedResult r;
    for (int i = 0; i < kFileCount; ++i) {
        const File& f = kFiles[i];
        if (!f.path || !f.data) continue;
        const std::wstring path = mod_dir + L"\\" + f.path;
        // Folders on the way (grips\...).
        for (size_t at = mod_dir.size() + 1; (at = path.find(L'\\', at)) != std::wstring::npos; ++at)
            CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
        // CREATE_NEW fails when the file exists, so a player's own file is
        // never replaced, even if two copies of the game start at once.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) ++r.kept;
            else ++r.failed;
            continue;
        }
        DWORD done = 0;
        const bool ok = WriteFile(h, f.data, f.size, &done, nullptr) && done == f.size;
        CloseHandle(h);
        if (ok) {
            ++r.written;
        } else {
            DeleteFileW(path.c_str());  // never leave half a file behind
            ++r.failed;
        }
    }
    return r;
}

}  // namespace holevr::builtin_files
