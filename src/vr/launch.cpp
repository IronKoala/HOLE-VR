#include "vr/launch.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "ue/process_event.h"

namespace holevr::vr::launch {
namespace {

using ue::UObject;

// The Blueprint events a real click on START runs (seen in the 2026-11-02
// menu study logs): the button's own handler for its inner Button's
// OnClicked, which calls the title screen's handler. A key press on the title
// screen runs the title screen's handler directly.
constexpr const char* kStartButtonClicked =
    "BndEvt__UIP_Button_Primary_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
constexpr const char* kTitleStartHandler =
    "BndEvt__UIM_Title_UIP_Button_Secondary_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature";
// START's widget class and name, and the title screen's class. The button
// class has a Blueprint Tick that runs every frame while it is on screen
// (menu study: "UIP_Button_Primary_C.Tick x584" per second).
constexpr const char* kButtonClass = "UIP_Button_Primary_C";
constexpr const char* kStartButtonName = "UIP_Btn_START";
constexpr const char* kTitleClass = "UIM_Title_C";

// If the player has been in the level this long and no title screen came
// up, stop watching.
constexpr double kGiveUpS = 90.0;

struct Settings {
    bool auto_start = true;
    double delay_s = 0.3;
} S;

double Now() { return GetTickCount64() / 1000.0; }

// Seconds since the game process started, for the log.
double SinceLaunch() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c{{created.dwLowDateTime, created.dwHighDateTime}};
    ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
    return (n.QuadPart - c.QuadPart) / 1e7;
}

bool CallBool(UObject obj, const char* fn) {
    ue::Call c(obj, fn);
    return c.Invoke() && c.Result<bool>();
}

void SetOpacity(UObject obj, float a) {
    ue::Call o(obj, "SetRenderOpacity");
    o.Set("InOpacity", a);
    o.Invoke();
}

// ------------------------------------------------------------ the window --

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

// ------------------------------------------------------- the title screen --

enum Stage {
    kWaiting,   // seen, waiting out the short delay
    kClicked,   // START button's click event called
    kHandler,   // title screen's START handler called directly
    kEnterKey,  // focused START and sent Enter, like a keyboard press
    kGaveUp,    // left for the player
};

std::atomic<bool> g_finished{false};       // first title screen dealt with (or none came)
std::atomic<bool> g_pressed{false};        // START pressed at least once
std::atomic<uint64_t> g_last_tick_ms{0};   // last frame START on the title ticked
std::atomic<uint64_t> g_seen_at_ms{0};     // first frame we saw the title on screen

// Game thread only (START's Tick).
struct Title {
    UObject obj = 0;
    UObject button = 0;
    double up_at = -1;
    double stage_at = 0;
    int stage = kWaiting;
} T;

// Runs after every UIP_Button_Primary_C.Tick, on the game thread. Only START
// on a title screen is of interest; only the first title screen is pressed.
void OnButtonTick(UObject self, UObject, void*) {
    if (g_finished.load(std::memory_order_relaxed)) return;
    if (ue::GetName(self) != kStartButtonName) return;
    UObject tree = ue::GetOuter(self);
    UObject title = tree ? ue::GetOuter(tree) : 0;
    if (!title || ue::GetName(ue::GetClass(title)) != kTitleClass) return;
    if (T.obj && T.obj != title) return;  // a later title screen: not ours

    const double now = Now();
    g_last_tick_ms = GetTickCount64();
    if (!T.obj) {
        if (!CallBool(title, "IsInViewport")) return;
        T.obj = title;
        T.button = self;
        T.up_at = T.stage_at = now;
        g_seen_at_ms = GetTickCount64();
        // See-through until it is gone, so it never shows. Put back if we
        // give up.
        SetOpacity(title, 0.0f);
        log::Info("launch: title screen %s came up %.1f s after launch; made it see-through, pressing START "
                  "(title_auto_start=0 turns this off)",
                  ue::GetName(title).c_str(), SinceLaunch());
    }
    switch (T.stage) {
        case kWaiting: {
            if (now - T.up_at < S.delay_s) return;
            ue::Call c(self, kStartButtonClicked);
            if (c.Valid()) {
                c.Invoke();
                log::Info("launch: pressed START (the button's click event)");
                T.stage_at = now;
            } else {
                log::Warn("launch: START has no click event, using the title's handler");
                T.stage_at = now - 10;  // straight on to the handler next frame
            }
            T.stage = kClicked;
            g_pressed = true;
            break;
        }
        case kClicked: {
            if (now - T.stage_at < 1.0) return;
            ue::Call c(title, kTitleStartHandler);
            if (c.Valid()) {
                c.Invoke();
                log::Info("launch: title screen still up, called its START handler directly");
            } else {
                log::Warn("launch: title screen has no %s", kTitleStartHandler);
            }
            T.stage = kHandler;
            T.stage_at = now;
            break;
        }
        case kHandler: {
            if (now - T.stage_at < 1.0) return;
            // Last resort: what a keyboard press does (focus START, Enter).
            ue::Call f(self, "SetFocus");
            f.Invoke();
            HWND w = GameWindow();
            if (w) {
                PostMessageW(w, WM_KEYDOWN, VK_RETURN, 1);
                PostMessageW(w, WM_KEYUP, VK_RETURN, (1u << 31) | (1u << 30) | 1);
            }
            log::Info("launch: title screen still up, sent Enter to START (window %d)", w ? 1 : 0);
            T.stage = kEnterKey;
            T.stage_at = now;
            break;
        }
        case kEnterKey: {
            if (now - T.stage_at < 2.0) return;
            SetOpacity(title, 1.0f);
            log::Error("launch: could not get past the title screen; press START yourself "
                       "(title_auto_start=0 turns this off)");
            T.stage = kGaveUp;
            g_finished = true;
            break;
        }
        default:
            break;
    }
}

// START's Tick function: the UFunction "Tick" of the UIP_Button_Primary_C
// Blueprint class.
UObject FindButtonTick() {
    UObject found = 0;
    ue::objects::ForEach([&](int, UObject o) {
        if (found) return;
        if (ue::GetName(o) != "Tick") return;
        UObject outer = ue::GetOuter(o);
        if (!outer || ue::GetName(outer) != kButtonClass) return;
        if (ue::GetName(ue::GetClass(o)) != "Function") return;
        found = o;
    });
    return found;
}

DWORD WINAPI WatchThread(LPVOID) {
    const double start = Now();
    double player_since = -1;
    // The game is still up when the player has been around kGiveUpS with no
    // title screen in sight: stop then.
    auto give_up = [&](const char* why) {
        if (game::Player()) {
            if (player_since < 0) player_since = Now();
            if (Now() - player_since > kGiveUpS && !g_seen_at_ms) {
                log::Info("launch: %s after %.0f s of play; no longer watching for the title screen", why, kGiveUpS);
                g_finished = true;
                return true;
            }
        }
        return false;
    };

    while (!ue::process_event::Ready()) {
        if (Now() - start > 300) return 0;  // game_hooks=0 or no ProcessEvent
        Sleep(50);
    }
    UObject uw = 0;
    while (!(uw = ue::FindObject("UserWidget /Script/UMG.Default__UserWidget"))) Sleep(100);
    if (!ue::process_event::HookVtableOf(uw)) {
        log::Error("launch: could not follow the game's widgets; the title screen is left alone");
        return 0;
    }
    UObject tick = 0;
    while (!(tick = FindButtonTick())) {
        if (give_up("no START button class loaded")) return 0;
        Sleep(250);
    }
    if (!ue::process_event::On(tick, nullptr, &OnButtonTick)) {
        log::Error("launch: could not follow START's tick; the title screen is left alone");
        return 0;
    }
    log::Info("launch: watching for the title screen (%.1f s after launch)", SinceLaunch());

    while (!g_finished) {
        Sleep(100);
        if (g_pressed) {
            // START stops ticking once its title screen is gone.
            uint64_t last = g_last_tick_ms.load();
            if (GetTickCount64() - last > 1000) {
                log::Info("launch: title screen closed %.1f s after it came up; you're in the game",
                          (last - g_seen_at_ms.load()) / 1000.0);
                g_finished = true;
            }
        } else if (give_up("no title screen came up")) {
            return 0;
        }
    }
    return 0;
}

}  // namespace

void Init() {
    S.auto_start = config::GetBool("title_auto_start", true);
    S.delay_s = config::GetFloat("title_auto_start_delay", 0.3f);
    log::Info("launch: %s", S.auto_start ? "START on the title screen is pressed for you the first time it comes up"
                                         : "title screen left alone (title_auto_start=0)");
    if (!S.auto_start) return;
    if (HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace holevr::vr::launch
