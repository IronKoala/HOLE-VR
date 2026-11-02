#include "vr/steamvr.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cwctype>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "vr/openxr.h"

namespace holevr::vr::steamvr {
namespace {

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Which of SteamVR's processes are running right now.
struct Processes {
    bool server = false, monitor = false, compositor = false, startup = false;
};

Processes Scan() {
    Processes p;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return p;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        std::wstring n = Lower(e.szExeFile);
        if (n == L"vrserver.exe") p.server = true;
        else if (n == L"vrmonitor.exe") p.monitor = true;
        else if (n == L"vrcompositor.exe") p.compositor = true;
        else if (n == L"vrstartup.exe") p.startup = true;
    }
    CloseHandle(snap);
    return p;
}

// SteamVR's install folder from the runtime library path
// (<SteamVR>\bin\vrclient_x64.dll or <SteamVR>\bin\win64\...), or "".
std::wstring SteamVrRoot(const std::wstring& runtime_lib) {
    std::wstring low = Lower(runtime_lib);
    size_t bin = low.rfind(L"\\bin\\");
    if (bin == std::wstring::npos) bin = low.rfind(L"/bin/");
    if (bin == std::wstring::npos) return {};
    return runtime_lib.substr(0, bin);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Asks Steam to run SteamVR (app 250820), exactly like its Launch button, so
// SteamVR runs as its own app and not as a child of the game.
bool LaunchThroughSteam() {
    // ShellExecute may hand the URL to shell extensions, which need COM.
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(com)) CoUninitialize();
    if (r > 32) {
        log::Info("steamvr: SteamVR was not running, asked Steam to start it");
        return true;
    }
    log::Warn("steamvr: Steam did not take the request to start SteamVR (error %lld)", static_cast<long long>(r));
    return false;
}

// Runs SteamVR's own starter, vrstartup.exe (what Steam runs for SteamVR).
bool LaunchDirect(const std::wstring& root) {
    std::wstring dir = root + L"\\bin\\win64";
    std::wstring exe = dir + L"\\vrstartup.exe";
    if (!FileExists(exe)) {
        log::Error("steamvr: %ls not found; start SteamVR yourself", exe.c_str());
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Outside the game's job if allowed, so it isn't tied to the game.
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             dir.c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
    if (!ok) {
        log::Error("steamvr: could not start %ls (error %lu); start SteamVR yourself", exe.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log::Info("steamvr: started %ls", exe.c_str());
    return true;
}

// Waits until SteamVR's monitor or compositor is running. True if it came up.
bool WaitUntilUp(DWORD limit_ms) {
    const DWORD start = GetTickCount();
    for (;;) {
        Sleep(500);
        Processes p = Scan();
        if (p.monitor || p.compositor) {
            // Give its headset driver a moment to find the headset.
            Sleep(1500);
            log::Info("steamvr: up after %.1f s (server %d, monitor %d, compositor %d)",
                      (GetTickCount() - start) / 1000.0, p.server, p.monitor, p.compositor);
            return true;
        }
        if (GetTickCount() - start > limit_ms) return false;
    }
}

}  // namespace

bool Running() {
    Processes p = Scan();
    return p.monitor || p.compositor;
}

void EnsureRunning() {
    if (!config::GetBool("start_steamvr", true)) {
        log::Info("steamvr: start_steamvr=0, leaving SteamVR to you");
        return;
    }
    std::wstring lib = xr::ActiveRuntimeLibrary();
    std::wstring root = SteamVrRoot(lib);
    if (lib.empty() || root.empty() || Lower(lib).find(L"vrclient") == std::wstring::npos) {
        log::Info("steamvr: the active OpenXR runtime is not SteamVR (%ls), not starting it",
                  lib.empty() ? L"none registered" : lib.c_str());
        return;
    }
    Processes p = Scan();
    if (p.monitor || p.compositor) {
        log::Info("steamvr: already running (server %d, monitor %d, compositor %d)", p.server, p.monitor,
                  p.compositor);
        return;
    }
    // Wait for SteamVR before the first OpenXR call: an early call would
    // start the bare server that refuses the game (see header).
    if (p.startup) {
        log::Info("steamvr: SteamVR is already starting");
        if (WaitUntilUp(30000)) return;
    }
    if (LaunchThroughSteam() && WaitUntilUp(20000)) return;
    if (!Running()) {
        log::Info("steamvr: not up yet, starting it directly");
        if (LaunchDirect(root) && WaitUntilUp(40000)) return;
    }
    if (!Running()) log::Warn("steamvr: SteamVR did not come up; trying the headset anyway");
}

}  // namespace holevr::vr::steamvr
