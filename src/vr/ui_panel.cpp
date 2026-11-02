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
// APlayerController::bShowMouseCursor every frame from the present thread;
// the controller pointer and its class are refreshed once a second.

std::atomic<uintptr_t> g_pc{0};
std::atomic<uintptr_t> g_pc_class{0};
int32_t g_cursor_offset = -1;
uint8_t g_cursor_mask = 0;
UObject g_pc_base_class = 0;
std::atomic<bool> g_menu_open{false};

bool ReadMenuOpen() {
    uintptr_t pc = g_pc.load();
    if (!pc || g_cursor_offset < 0) return false;
    if (mem::ReadOr<uintptr_t>(pc + ue::off::kObjClass) != g_pc_class.load()) return false;
    return (mem::ReadOr<uint8_t>(pc + g_cursor_offset) & g_cursor_mask) != 0;
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
    void AddLayers(XrSession, XrSpace, XrTime, std::vector<const XrCompositionLayerBaseHeader*>&) override {
        bool open = ReadMenuOpen();
        if (open != g_menu_open.load()) {
            g_menu_open = open;
            log::Info("menu panel: menu %s", open ? "opened" : "closed");
        }
        input::Snapshot in = input::Peek();
        HWND w = GameWindow();

        // Menu button on either controller toggles the pause menu (Escape).
        // A half-second guard so a double press doesn't open and close it.
        static DWORD s_last_menu = 0;
        for (int hnd = 0; hnd < 2; ++hnd) {
            bool m = in.valid && in.hand[hnd].menu.down;
            if (S.pause_button && m && !PT.menu_down[hnd] && w && GetTickCount() - s_last_menu > 500) {
                PostKey(w, VK_ESCAPE);
                s_last_menu = GetTickCount();
            }
            PT.menu_down[hnd] = m;
        }
        if (!open) return;
        // B/Y goes back (Escape).
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

void Maintain() {
    if (!ue::reflection::Ready()) return;
    if (!g_pc_base_class) {
        g_pc_base_class = ue::FindObject("Class /Script/Engine.PlayerController");
        if (!g_pc_base_class) return;
        auto prop = ue::reflection::FindProperty(g_pc_base_class, "bShowMouseCursor");
        if (!prop.valid() || !prop.bool_mask) {
            log::Warn("menu panel: PlayerController.bShowMouseCursor not found; menus stay on the monitor");
            g_pc_base_class = 0;
            return;
        }
        g_cursor_offset = prop.offset;
        g_cursor_mask = prop.bool_mask;
    }
    // The player's controller when there is a player, else any live one (title screen).
    UObject pc = 0;
    if (UObject player = game::Player()) ue::GetProp(player, "Controller", pc);
    uintptr_t cur = g_pc.load();
    if (!pc && cur && mem::ReadOr<uintptr_t>(cur + ue::off::kObjClass) == g_pc_class.load()) return;
    if (!pc) pc = ue::FindFirstInstance(g_pc_base_class);
    if (pc != cur) {
        g_pc_class = pc ? ue::GetClass(pc) : 0;
        g_pc = pc;
        if (pc) log::Info("menu panel: watching %s", ue::GetName(ue::GetClass(pc)).c_str());
    }
}

bool MenuOpen() { return g_menu_open.load(); }

}  // namespace holevr::vr::ui_panel
