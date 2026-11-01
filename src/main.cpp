// HOLE VR entry point. Loaded by the game as dxgi.dll (see proxy/dxgi_proxy.cpp).

#include <windows.h>

#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "proxy/dxgi_proxy.h"
#include "ue/dumper.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"

namespace {

constexpr const char* kVersion = "0.1.0-foundation";
HMODULE g_self = nullptr;

std::wstring ModuleDir(HMODULE m) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring s(buf, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The build string the engine embeds, e.g. "++UE5+Release-5.5-CL-40574608".
std::string EngineBuildString() {
    const auto& game = holevr::mem::Game();
    const wchar_t needle[] = L"++UE5+Release-";
    for (const auto& s : game.sections) {
        if (s.name != ".rdata") continue;
        uintptr_t hit = holevr::mem::FindBytes(s.start, s.size, needle, sizeof(needle) - 2);
        if (hit) return Narrow(reinterpret_cast<const wchar_t*>(hit));
    }
    return "unknown";
}

bool WaitFor(const char* what, bool (*fn)(), int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 250) {
        if (fn()) return true;
        Sleep(250);
    }
    holevr::log::Error("Timed out waiting for %s", what);
    return false;
}

DWORD WINAPI InitThread(LPVOID) {
    using namespace holevr;
    std::wstring mod_dir = ModuleDir(g_self) + L"\\hole-vr";
    CreateDirectoryW(mod_dir.c_str(), nullptr);
    log::Init(mod_dir);
    log::Info("HOLE VR %s starting", kVersion);
    config::Load(mod_dir);
    log::Info("Game module at %p, size 0x%zx, engine build %s",
              reinterpret_cast<void*>(mem::Game().base), mem::Game().size,
              EngineBuildString().c_str());

    const int timeout = config::GetInt("engine_wait_seconds", 90) * 1000;
    if (!WaitFor("FNamePool", ue::names::Init, timeout)) return 0;
    if (!WaitFor("GUObjectArray", ue::objects::Init, timeout)) return 0;
    if (!WaitFor("reflection offsets", ue::reflection::Init, timeout)) return 0;
    log::Info("Engine discovery complete");

    if (config::GetBool("dump_on_start", true)) ue::dumper::DumpAll(mod_dir);
    log::Info("Press Ctrl+F9 in game to write a fresh dump (after loading a level, "
              "so weapon and player classes are included)");

    // Development hotkeys. Polled rather than hooked so nothing here can
    // interfere with the game's own input.
    bool was_down = false;
    for (;;) {
        bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (down && !was_down) {
            log::Info("Ctrl+F9: dumping (%d live objects)", ue::objects::Count());
            ue::dumper::DumpAll(mod_dir);
        }
        was_down = down;
        Sleep(100);
    }
}

bool IsTheGame() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf, n);
    return exe.find(L"HOLE-Win64-Shipping") != std::wstring::npos;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        if (!holevr::proxy::LoadRealDxgi()) {
            MessageBoxW(nullptr, L"HOLE VR could not load the system dxgi.dll.", L"HOLE VR",
                        MB_ICONERROR);
            return FALSE;
        }
        // Only start the mod inside the game itself, never in crash reporters
        // or other tools that happen to load this dxgi.dll.
        if (IsTheGame()) {
            if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
        }
    }
    return TRUE;
}
