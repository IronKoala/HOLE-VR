// HOLE VR entry point. Loaded by the game as dxgi.dll (see proxy/dxgi_proxy.cpp).

#include <windows.h>

#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "core/memory.h"
#include "game/game.h"
#include "game/weapon_lab.h"
#include "proxy/dxgi_proxy.h"
#include "version.h"
#include "ue/dumper.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/process_event.h"
#include "ue/reflection.h"
#include "vr/dxgi_hooks.h"
#include "vr/body/body.h"
#include "vr/controls.h"
#include "vr/input.h"
#include "vr/openxr.h"
#include "vr/cvars.h"
#include "vr/render_fixes.h"
#include "vr/ui_panel.h"
#include "vr/wrist_hud.h"
#include "vr/world_menu.h"
#include "vr/menu_study.h"
#include "vr/launch.h"
#include "vr/stereo.h"

namespace {

constexpr const char* kVersion = HOLEVR_VERSION_STR;
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

std::wstring g_mod_dir;

// Starts OpenXR and takes over the engine's stereo device. Waits quietly for
// SteamVR/the headset if they are not running yet.
void StartVr() {
    using namespace holevr;
    vr::input::Init();  // controller actions must exist before the session starts
    vr::ui_panel::Init();
    vr::world_menu::Init();
    vr::wrist_hud::Init();
    bool told = false;
    while (!vr::xr::InitInstance()) {
        if (!told) log::Info("Waiting for SteamVR and the headset...");
        told = true;
        Sleep(2000);
    }
    for (int tries = 0; !vr::stereo::Install(); ++tries) {
        if (tries == 120) {
            log::Error("Gave up hooking the stereo device");
            return;
        }
        Sleep(500);
    }
}

DWORD WINAPI InitThread(LPVOID) {
    using namespace holevr;
    const std::wstring& mod_dir = g_mod_dir;
    log::Info("Game module at %p, size 0x%zx, engine build %s",
              reinterpret_cast<void*>(mem::Game().base), mem::Game().size,
              EngineBuildString().c_str());

    const int timeout = config::GetInt("engine_wait_seconds", 90) * 1000;
    if (!WaitFor("FNamePool", ue::names::Init, timeout)) return 0;
    if (!WaitFor("GUObjectArray", ue::objects::Init, timeout)) return 0;
    if (!WaitFor("reflection offsets", ue::reflection::Init, timeout)) return 0;
    log::Info("Engine discovery complete");
    // Before the slow start-up work: the title screen is up within seconds.
    vr::launch::Init();

    if (config::GetBool("dump_on_start", true)) ue::dumper::DumpAll(mod_dir);
    const bool render_fixes = config::GetBool("vr_render_fixes", true) && vr::render_fixes::Init();

    log::Info("Press Ctrl+F9 in game to write a fresh dump (after loading a level, "
              "so weapon and player classes are included)");
    if (config::GetBool("vr_enabled", true)) {
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { StartVr(); return 0; }, nullptr, 0, nullptr))
            CloseHandle(t);
    }

    // Game hooks: a game-thread tick and the ability to call game functions.
    // game_hooks=0 in hole-vr.ini turns this off if it ever causes trouble.
    const bool hooks = config::GetBool("game_hooks", true) &&
                       WaitFor("ProcessEvent", ue::process_event::Init, timeout) && game::Init();
    if (hooks) {
        game::weapon_lab::Init();
        vr::controls::Init();
        vr::body::Init();  // after controls: reads this frame's hand poses
        vr::controls::InitLate();  // after the body: holsters ride this frame's body pose
        vr::cvars::Init();
        vr::wrist_hud::InitGame();
        vr::world_menu::InitGame();
        vr::menu_study::InitGame();
    }

    // Development hotkeys. Polled rather than hooked so nothing here can
    // interfere with the game's own input.
    bool dump_was_down = false, recenter_was_down = false, aa_was_down = false;
    // One automatic dump per launch once the game's own classes (weapons,
    // player) are loaded, so the class list always covers them without a
    // keypress (the start-up dump runs before any level and misses them).
    int game_classes_seen_at = -1;
    bool ingame_dumped = !config::GetBool("dump_in_game", true);
    // Time spent on each job of this loop ("= N ms/s" in the perf lines is
    // its share of one CPU core). It used to spend most of every second
    // rescanning every game object (2026-11-03 performance pass).
    static perf::Counter bg_maintain{"background: game classes"}, bg_render{"background: render fixes"},
        bg_panel{"background: menu panel"}, bg_menu{"background: menu scan"}, bg_study{"background: menu study"},
        bg_cvars{"background: cvars"};
    for (perf::Counter* c : {&bg_maintain, &bg_render, &bg_panel, &bg_menu, &bg_study, &bg_cvars}) {
        c->per_second_total = true;
        perf::Register(c);
    }
    for (int frame = 0;; ++frame) {
        if (hooks && !ingame_dumped && frame % 60 == 45 && game::GunBaseClass()) {
            if (game_classes_seen_at < 0) game_classes_seen_at = frame;
            else if (frame - game_classes_seen_at >= 600) {   // ~10 s after the level loaded
                ingame_dumped = true;
                log::Info("dump: in-game class dump (once per launch, dump_in_game=0 turns it off)");
                ue::dumper::DumpAll(mod_dir);
            }
        }
        if (hooks) {
            game::weapon_lab::PollHotkeys();
            if (frame % 60 == 0) { perf::Scope t(bg_maintain); game::Maintain(); }
            if (frame % 60 == 30) { perf::Scope t(bg_cvars); vr::cvars::Maintain(); }
        }
        // New cameras, volumes and captures appear on every level load, so
        // the render fixes are re-applied once a second.
        if (render_fixes && frame % 60 == 0) { perf::Scope t(bg_render); vr::render_fixes::Apply(); }
        if (frame % 60 == 15) { perf::Scope t(bg_panel); vr::ui_panel::Maintain(); }
        if (frame % 15 == 7) { perf::Scope t(bg_menu); vr::world_menu::Scan(); }
        if (hooks) { perf::Scope t(bg_study); vr::menu_study::Poll(); }
        bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
        bool dump = ctrl && (GetAsyncKeyState(VK_F9) & 0x8000);
        bool recenter = ctrl && (GetAsyncKeyState(VK_F8) & 0x8000);
        bool cycle_aa = ctrl && (GetAsyncKeyState(VK_F10) & 0x8000);
        if (hooks && cycle_aa && !aa_was_down) vr::cvars::CycleAntiAliasing();
        aa_was_down = cycle_aa;
        if (dump && !dump_was_down) {
            log::Info("Ctrl+F9: dumping (%d live objects)", ue::objects::Count());
            ue::dumper::DumpAll(mod_dir);
        }
        if (recenter && !recenter_was_down) vr::stereo::Recenter();
        dump_was_down = dump;
        recenter_was_down = recenter;
        Sleep(16);
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
            // Logging, settings and the command-line hook must be ready
            // before the engine's own startup code runs.
            g_mod_dir = ModuleDir(inst) + L"\\hole-vr";
            CreateDirectoryW(g_mod_dir.c_str(), nullptr);
            holevr::log::Init(g_mod_dir);
            holevr::log::Info("HOLE VR %s starting", kVersion);
            holevr::config::Load(g_mod_dir);
            holevr::proxy::SetFactoryCallback(&holevr::vr::dxgi_hooks::OnFactoryCreated);
            holevr::vr::stereo::InstallCommandLineHook();
            if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
        }
    }
    return TRUE;
}
