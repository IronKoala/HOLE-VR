// HOLE VR installer. A single exe with the mod's dxgi.dll embedded.
//
// Finds HOLE through Steam, copies dxgi.dll next to HOLE-Win64-Shipping.exe,
// backs up any other dxgi.dll that was there (ReShade and similar tools use
// the same name), and puts it back on uninstall.
//
// Command line (for scripts): /install, /uninstall, /purge (also delete
// settings and logs), /path=<HOLE folder>, /quiet (no message boxes).
// Exit code 0 on success.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "version.h"

namespace {

constexpr int kDllResource = 101;
constexpr wchar_t kExeName[] = L"HOLE-Win64-Shipping.exe";
constexpr wchar_t kBackupSuffix[] = L".hole-vr-backup";
constexpr wchar_t kTitle[] = L"HOLE VR Setup";

enum : int { ID_INSTALL = 1001, ID_UNINSTALL, ID_BROWSE, ID_CLOSE, ID_PURGE, ID_STATUS, ID_PATH };

std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring TrimSlash(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

// Accepts the Steam "HOLE" folder, its inner "HOLE" folder, or Binaries\Win64,
// and returns the folder holding the game exe (where dxgi.dll must go).
std::wstring ResolveBinDir(const std::wstring& any) {
    std::wstring p = TrimSlash(any);
    for (const wchar_t* sub : {L"", L"\\Binaries\\Win64", L"\\HOLE\\Binaries\\Win64"}) {
        std::wstring dir = p + sub;
        if (FileExists(dir + L"\\" + kExeName)) return dir;
    }
    return {};
}

std::wstring ReadRegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// Steam library roots: the Steam folder itself plus every "path" entry in
// steamapps\libraryfolders.vdf.
std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> libs;
    std::wstring steam = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty())
        steam = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return libs;
    steam = TrimSlash(steam);
    libs.push_back(steam);

    std::wstring vdf = steam + L"\\steamapps\\libraryfolders.vdf";
    HANDLE h = CreateFileW(vdf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return libs;
    std::string text;
    char buf[4096];
    DWORD got;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    CloseHandle(h);

    // Lines look like:   "path"		"D:\\SteamLibrary"
    size_t pos = 0;
    while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
        pos += 6;
        size_t a = text.find('"', pos);
        if (a == std::string::npos) break;
        size_t b = text.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string raw = text.substr(a + 1, b - a - 1), path;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;
            path += raw[i];
        }
        libs.push_back(TrimSlash(Widen(path.c_str())));
        pos = b + 1;
    }
    return libs;
}

std::wstring FindGame() {
    // Running the installer from inside the game folder wins.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring here(self);
    here = here.substr(0, here.find_last_of(L'\\'));
    if (auto d = ResolveBinDir(here); !d.empty()) return d;
    for (const auto& lib : SteamLibraries())
        if (auto d = ResolveBinDir(lib + L"\\steamapps\\common\\HOLE"); !d.empty()) return d;
    return {};
}

// Reads a string value from a file's version resource by scanning the raw
// file. GetFileVersionInfo and LoadLibraryEx are not used because a file
// named dxgi.dll can be answered with the system DLL's details instead.
std::wstring VersionString(const std::wstring& file, const wchar_t* field) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::vector<char> data;
    if (size.QuadPart > 0 && size.QuadPart < (64 << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
        data.resize(got);
    }
    CloseHandle(h);
    // Version strings are stored as: key, NUL, padding NULs, value, NUL (UTF-16).
    std::wstring key(field);
    const size_t key_bytes = (key.size() + 1) * sizeof(wchar_t);
    for (size_t i = 0; i + key_bytes <= data.size(); i += 2) {
        if (std::memcmp(data.data() + i, key.c_str(), key_bytes) != 0) continue;
        size_t j = i + key_bytes;
        while (j + 1 < data.size() && data[j] == 0 && data[j + 1] == 0) j += 2;
        std::wstring out;
        for (; j + 1 < data.size() && out.size() < 128; j += 2) {
            wchar_t c = static_cast<unsigned char>(data[j]) | (static_cast<unsigned char>(data[j + 1]) << 8);
            if (!c) break;
            out += c;
        }
        return out;
    }
    return {};
}

std::wstring ProductName(const std::wstring& file) { return VersionString(file, L"ProductName"); }
std::wstring FileVersionString(const std::wstring& file) { return VersionString(file, L"ProductVersion"); }

bool IsOurs(const std::wstring& dll) { return ProductName(dll) == Widen(HOLEVR_PRODUCT_NAME); }

bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kExeName) == 0) found = true;
    CloseHandle(snap);
    return found;
}

struct Result {
    bool ok;
    std::wstring message;
};

Result Install(const std::wstring& bin) {
    if (GameRunning()) return {false, L"HOLE is running. Close the game, then try again."};
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kDllResource), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = mem ? LockResource(mem) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!bytes || !size) return {false, L"This installer is damaged (the mod file is missing)."};

    std::wstring dll = bin + L"\\dxgi.dll";
    std::wstring note;
    if (FileExists(dll) && !IsOurs(dll)) {
        // Another tool's dxgi.dll (ReShade, a frame limiter...). Keep it safe.
        std::wstring backup = dll + kBackupSuffix;
        for (int i = 2; FileExists(backup); ++i) backup = dll + kBackupSuffix + std::to_wstring(i);
        if (!MoveFileW(dll.c_str(), backup.c_str()))
            return {false, L"Could not back up the existing dxgi.dll (error " +
                               std::to_wstring(GetLastError()) + L")."};
        note = L"\n\nAnother dxgi.dll was already there. It was saved as " +
               backup.substr(backup.find_last_of(L'\\') + 1) + L" and comes back if you uninstall.";
    }

    std::wstring tmp = dll + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {false, L"Could not write to the game folder (error " + std::to_wstring(GetLastError()) +
                           L"). Try running the installer as administrator."};
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes, size, &written, nullptr) && written == size;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        return {false, L"Could not install dxgi.dll (error " + std::to_wstring(err) + L")."};
    }
    return {true, L"HOLE VR " + Widen(HOLEVR_VERSION_STR) +
                      L" is installed.\n\nStart SteamVR, then launch HOLE from Steam as usual." + note};
}

bool DeleteFolder(const std::wstring& dir) {
    // Sent to the Recycle Bin, so settings can still be recovered.
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_ALLOWUNDO;
    return SHFileOperationW(&op) == 0;
}

Result Uninstall(const std::wstring& bin, bool purge) {
    if (GameRunning()) return {false, L"HOLE is running. Close the game, then try again."};
    std::wstring dll = bin + L"\\dxgi.dll";
    bool removed = false;
    if (FileExists(dll)) {
        if (!IsOurs(dll))
            return {false, L"The dxgi.dll in the game folder is not HOLE VR, so it was left alone."};
        if (!DeleteFileW(dll.c_str()))
            return {false, L"Could not remove dxgi.dll (error " + std::to_wstring(GetLastError()) + L")."};
        removed = true;
    }
    std::wstring msg = removed ? L"HOLE VR was removed." : L"HOLE VR was not installed.";
    std::wstring backup = dll + kBackupSuffix;
    if (FileExists(backup) && !FileExists(dll) && MoveFileW(backup.c_str(), dll.c_str()))
        msg += L"\n\nYour previous dxgi.dll was put back.";
    std::wstring data = bin + L"\\hole-vr";
    if (DirExists(data)) {
        if (purge)
            msg += DeleteFolder(data) ? L"\n\nSettings and logs were moved to the Recycle Bin."
                                      : L"\n\nCould not delete the hole-vr settings folder.";
        else
            msg += L"\n\nYour settings were kept in the hole-vr folder.";
    }
    return {true, msg};
}

// ---------------------------------------------------------------- window ---

HWND g_wnd, g_status, g_path, g_install, g_uninstall, g_purge;
HFONT g_font, g_bold;
std::wstring g_bin;

void Refresh() {
    std::wstring status;
    if (g_bin.empty()) {
        SetWindowTextW(g_path, L"HOLE was not found. Click \"Choose folder...\" and pick the HOLE game folder.");
        status = L"";
    } else {
        SetWindowTextW(g_path, (L"Game folder: " + g_bin).c_str());
        std::wstring dll = g_bin + L"\\dxgi.dll";
        if (!FileExists(dll)) status = L"HOLE VR is not installed.";
        else if (IsOurs(dll)) {
            std::wstring v = FileVersionString(dll);
            status = L"HOLE VR " + v + L" is installed.";
            if (v != Widen(HOLEVR_VERSION_STR))
                status += L" This installer has version " + Widen(HOLEVR_VERSION_STR) + L".";
        } else
            status = L"Another dxgi.dll is in the game folder (for example ReShade). "
                     L"It will be backed up and restored if you uninstall.";
    }
    SetWindowTextW(g_status, status.c_str());
    bool ours = !g_bin.empty() && IsOurs(g_bin + L"\\dxgi.dll");
    EnableWindow(g_install, !g_bin.empty());
    SetWindowTextW(g_install, ours ? L"Update" : L"Install");
    EnableWindow(g_uninstall, ours);
    EnableWindow(g_purge, ours);
}

void Browse() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Pick the HOLE game folder (steamapps\\common\\HOLE).";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    bool got = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!got) return;
    std::wstring bin = ResolveBinDir(path);
    if (bin.empty()) {
        MessageBoxW(g_wnd, L"That folder does not contain HOLE (HOLE-Win64-Shipping.exe was not found).",
                    kTitle, MB_ICONWARNING);
        return;
    }
    g_bin = bin;
    Refresh();
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
          HFONT font, UINT dpi) {
    auto s = [dpi](int v) { return MulDiv(v, dpi, 96); };
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(w), s(h), g_wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = w;
        UINT dpi = GetDpiForWindow(w);
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        bold.lfHeight = bold.lfHeight * 3 / 2;
        g_bold = CreateFontIndirectW(&bold);
        Make(L"STATIC", (L"HOLE VR " + Widen(HOLEVR_VERSION_STR)).c_str(), 0, 16, 14, 440, 26, 0, g_bold, dpi);
        g_path = Make(L"STATIC", L"", SS_PATHELLIPSIS, 16, 48, 440, 20, ID_PATH, g_font, dpi);
        g_status = Make(L"STATIC", L"", 0, 16, 74, 440, 40, ID_STATUS, g_font, dpi);
        g_purge = Make(L"BUTTON", L"When uninstalling, also delete settings and logs", BS_AUTOCHECKBOX,
                       16, 120, 440, 22, ID_PURGE, g_font, dpi);
        g_install = Make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 156, 100, 30, ID_INSTALL,
                         g_font, dpi);
        g_uninstall = Make(L"BUTTON", L"Uninstall", WS_TABSTOP, 124, 156, 100, 30, ID_UNINSTALL, g_font, dpi);
        Make(L"BUTTON", L"Choose folder...", WS_TABSTOP, 232, 156, 120, 30, ID_BROWSE, g_font, dpi);
        Make(L"BUTTON", L"Close", WS_TABSTOP, 360, 156, 96, 30, ID_CLOSE, g_font, dpi);
        Refresh();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_INSTALL: {
            Result r = Install(g_bin);
            MessageBoxW(w, r.message.c_str(), kTitle, r.ok ? MB_ICONINFORMATION : MB_ICONERROR);
            Refresh();
            return 0;
        }
        case ID_UNINSTALL: {
            bool purge = SendMessageW(g_purge, BM_GETCHECK, 0, 0) == BST_CHECKED;
            Result r = Uninstall(g_bin, purge);
            MessageBoxW(w, r.message.c_str(), kTitle, r.ok ? MB_ICONINFORMATION : MB_ICONERROR);
            Refresh();
            return 0;
        }
        case ID_BROWSE: Browse(); return 0;
        case ID_CLOSE: DestroyWindow(w); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int RunCommandLine(int argc, wchar_t** argv) {
    bool install = false, purge = false, quiet = false;
    std::wstring path;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > 1 && (a[0] == L'/' || a[0] == L'-')) a = a.substr(1);
        if (_wcsicmp(a.c_str(), L"install") == 0) install = true;
        else if (_wcsicmp(a.c_str(), L"purge") == 0) purge = true;
        else if (_wcsicmp(a.c_str(), L"quiet") == 0) quiet = true;
        else if (_wcsnicmp(a.c_str(), L"path=", 5) == 0) path = a.substr(5);
    }
    std::wstring bin = path.empty() ? FindGame() : ResolveBinDir(path);
    Result r{false, L"HOLE was not found. Pass /path=\"<HOLE folder>\"."};
    if (!bin.empty()) r = install ? Install(bin) : Uninstall(bin, purge);
    if (!quiet) MessageBoxW(nullptr, r.message.c_str(), kTitle, r.ok ? MB_ICONINFORMATION : MB_ICONERROR);
    return r.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (_wcsicmp(argv[i] + (argv[i][0] == L'/' || argv[i][0] == L'-'), L"install") == 0 ||
            _wcsicmp(argv[i] + (argv[i][0] == L'/' || argv[i][0] == L'-'), L"uninstall") == 0)
            return RunCommandLine(argc, argv);
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_bin = FindGame();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"HoleVrSetup";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(472, dpi, 96), MulDiv(202, dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                               (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
