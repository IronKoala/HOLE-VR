#include "vr/ui_panel.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/dxgi_hooks.h"
#include "vr/input.h"
#include "vr/openxr.h"
#include "vr/world_menu.h"

namespace holevr::vr::ui_panel {
namespace {
using ue::UObject;

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(xr::GetProc(name));
}

// ------------------------------------------------------------- settings --

struct Settings {
    bool pause_button = true;  // controller menu button sends Escape
} S;

// ----------------------------------------------------- menu detection --
// The game shows the mouse cursor whenever a menu wants mouse input. We read
// APlayerController::bShowMouseCursor on the GAME thread, from the player's
// tick, straight off the player's current controller, and hand the answer to
// the present thread as a flag.
//
// It used to be read every frame from the present thread through a
// controller pointer refreshed once a second. On "Return to Title" the game
// frees the old level's controller while the headset keeps drawing, and that
// read hit the freed memory: the game's "Fatal Error!" (owner's run
// 2026-11-02 21:14 local; crash in UiListener::AddLayers on the RHI thread).
// The present thread now never touches game objects.

std::atomic<int32_t> g_cursor_offset{-1};
std::atomic<uint8_t> g_cursor_mask{0};
UObject g_pc_base_class = 0;  // background thread only (Maintain)
std::atomic<UObject> g_pc_class_ok{0};  // a controller class known to be a PlayerController
std::atomic<bool> g_cursor_shown{false};
std::atomic<uint64_t> g_cursor_read_ms{0};
std::atomic<bool> g_menu_open{false};

// A reading older than this (loading, title screen, paused: the player isn't
// ticking) counts as "no cursor menu". The pause menu and the other forearm
// menus are covered by world_menu::MenuUp() instead.
constexpr uint64_t kCursorStaleMs = 500;

bool ReadMenuOpen() {
    if (GetTickCount64() - g_cursor_read_ms.load() > kCursorStaleMs) return false;
    return g_cursor_shown.load();
}

// Game thread, after the player's Blueprint tick.
void OnPlayerTick(UObject player, float) {
    int32_t offset = g_cursor_offset.load(std::memory_order_acquire);
    if (offset < 0 || !player) return;
    UObject pc = 0;
    bool shown = false;
    if (ue::GetProp(player, "Controller", pc) && pc) {
        UObject cls = ue::GetClass(pc);
        if (cls && cls != g_pc_class_ok.load() && g_pc_base_class && ue::reflection::IsA(pc, g_pc_base_class))
            g_pc_class_ok = cls;
        if (cls && cls == g_pc_class_ok.load())
            shown = (mem::ReadOr<uint8_t>(pc + offset) & g_cursor_mask.load()) != 0;
    }
    g_cursor_shown = shown;
    g_cursor_read_ms = GetTickCount64();
}

// --------------------------------------------------------------- window --

HWND g_hwnd = nullptr;

BOOL CALLBACK FindWindowProc(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    g_hwnd = w;
    return FALSE;
}

HWND GameWindow() {
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = nullptr;
        EnumWindows(FindWindowProc, 0);
    }
    return g_hwnd;
}

// ------------------------------------------------------------- listener --
// Only menu detection and the buttons live here. The game's menus themselves
// are shown on the forearm by vr/world_menu; nothing here ever shows a copy
// of the screen.

void PostKey(HWND w, WPARAM vk) {
    PostMessageW(w, WM_KEYDOWN, vk, 1);
    PostMessageW(w, WM_KEYUP, vk, (1u << 31) | (1u << 30) | 1);
}

struct Buttons {
    bool back_down[2] = {};
    bool menu_down[2] = {};
} PT;

struct UiListener : xr::Listener {
    // Present thread: reads only flags the game thread set, never game memory.
    void AddLayers(XrSession, XrSpace, XrTime, std::vector<const XrCompositionLayerBaseHeader*>&) override {
        bool open = ReadMenuOpen();
        if (open != g_menu_open.load()) {
            g_menu_open = open;
            log::Info("menu panel: menu %s", open ? "opened" : "closed");
        }
        input::Snapshot in = input::Peek();
        HWND w = GameWindow();

        // Menu button on either controller toggles the pause menu: Escape
        // opens it (the game only opens it with that), and with the pause
        // menu on top the game thread closes it (world_menu). In a sub-menu
        // it goes back a step, like B/Y. A half-second guard so a double
        // press doesn't open and close it.
        static DWORD s_last_menu = 0;
        for (int hnd = 0; hnd < 2; ++hnd) {
            bool m = in.valid && in.hand[hnd].menu.down;
            if (S.pause_button && m && !PT.menu_down[hnd] && w && GetTickCount() - s_last_menu > 500) {
                if (world_menu::PauseMenuOnTop()) world_menu::RequestClosePause();
                else PostKey(w, VK_ESCAPE);
                s_last_menu = GetTickCount();
            }
            PT.menu_down[hnd] = m;
        }
        // The cursor never shows for the VR pause menu, so a menu on the
        // forearm counts too; before this, B/Y did nothing in a forearm menu.
        if (!open && !world_menu::MenuUp()) {
            // Only a press that starts while a menu is up counts.
            for (int hnd = 0; hnd < 2; ++hnd) PT.back_down[hnd] = in.valid && in.hand[hnd].secondary.down;
            return;
        }
        // B/Y goes back (Escape): out of Options to the pause menu, and so on.
        if (in.valid) {
            for (int hnd = 0; hnd < 2; ++hnd) {
                bool back = in.hand[hnd].secondary.down;
                if (back && !PT.back_down[hnd] && w) PostKey(w, VK_ESCAPE);
                PT.back_down[hnd] = back;
            }
        }
    }
} g_listener;

}  // namespace

void Init() {
    S.pause_button = config::GetBool("menu_button_pauses", true);
    xr::RegisterListener(&g_listener);
    log::Info("menu buttons: menu button %s; menus are never shown as a copy of the screen",
              S.pause_button ? "pauses" : "off");
}

void InitGame() {
    game::AddTickListener(&OnPlayerTick);
    log::Info("menu panel: menu cursor read on the game thread (player tick)");
}

void Maintain() {
    if (!ue::reflection::Ready() || g_cursor_offset.load() >= 0) return;
    if (!g_pc_base_class) g_pc_base_class = ue::FindObject("Class /Script/Engine.PlayerController");
    if (!g_pc_base_class) return;
    auto prop = ue::reflection::FindProperty(g_pc_base_class, "bShowMouseCursor");
    if (!prop.valid() || !prop.bool_mask) {
        static bool s_warned = false;
        if (!s_warned) log::Warn("menu panel: PlayerController.bShowMouseCursor not found; cursor menus not detected");
        s_warned = true;
        return;
    }
    g_cursor_mask.store(prop.bool_mask, std::memory_order_relaxed);
    g_cursor_offset.store(prop.offset, std::memory_order_release);
    log::Info("menu panel: PlayerController.bShowMouseCursor at +0x%x", prop.offset);
}

// The cursor never shows for the VR pause menu, so a menu on the forearm
// counts as open too (stops the controls firing while you press buttons).
bool MenuOpen() { return g_menu_open.load() || world_menu::MenuUp(); }

}  // namespace holevr::vr::ui_panel
