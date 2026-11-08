#include "vr/launch.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

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
constexpr const char* kStartButtonName = "UIP_Btn_START";
constexpr const char* kTitleClass = "UIM_Title_C";
// The game's button widgets (UIP_Button_Primary_C, _Secondary_C, ...) have a
// Blueprint Tick that runs every frame while they are on screen (menu study:
// "UIP_Button_Primary_C.Tick x584" per second). The title screen's buttons
// tick while it is up, so their Tick is our game-thread hook into it.
constexpr const char* kButtonClassPrefix = "UIP_Button";

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
std::atomic<uint64_t> g_last_tick_ms{0};   // last frame a title screen button ticked
std::atomic<uint64_t> g_seen_at_ms{0};     // first frame we saw the title on screen
std::atomic<int> g_ticks_seen{0};          // button ticks seen at all (diagnostics)

// Game thread only (the buttons' Tick).
struct Title {
    UObject obj = 0;
    UObject start = 0;
    double up_at = -1;
    double stage_at = 0;
    int stage = kWaiting;
} T;
int g_logged_buttons = 0;

// START in a title screen: its widget variable, else a button named
// UIP_Btn_START in the title's widget tree (the ticking button may be it).
UObject StartOf(UObject title, UObject ticking) {
    UObject btn = 0;
    if (ue::GetProp(title, kStartButtonName, btn) && btn) return btn;
    if (ue::GetName(ticking) == kStartButtonName) return ticking;
    return 0;
}

// Runs after every game button's Tick, on the game thread. Buttons on a title
// screen are of interest; only the first title screen is pressed.
void OnButtonTick(UObject self, UObject function, void*) {
    if (g_finished.load(std::memory_order_relaxed)) return;
    ++g_ticks_seen;
    UObject tree = ue::GetOuter(self);
    UObject owner = tree ? ue::GetOuter(tree) : 0;
    const bool on_title = owner && ue::GetName(ue::GetClass(owner)) == kTitleClass;
    if (g_logged_buttons < 6) {
        ++g_logged_buttons;
        log::Info("launch: %s.%s ticking on %s (in %s)%s", ue::GetName(ue::GetOuter(function)).c_str(),
                  ue::GetName(function).c_str(), ue::GetName(self).c_str(),
                  owner ? ue::GetName(owner).c_str() : "?", on_title ? ", on the title screen" : "");
    }
    if (!on_title) return;
    if (T.obj && T.obj != owner) return;  // a later title screen: not ours

    const double now = Now();
    g_last_tick_ms = GetTickCount64();
    if (!T.obj) {
        T.obj = owner;
        T.start = StartOf(owner, self);
        T.up_at = T.stage_at = now;
        g_seen_at_ms = GetTickCount64();
        // See-through until it is gone, so it never shows. Put back if we
        // give up.
        SetOpacity(owner, 0.0f);
        log::Info("launch: title screen %s is up, %.1f s after launch; made it see-through, pressing START "
                  "(START button %s; title_auto_start=0 turns this off)",
                  ue::GetName(owner).c_str(), SinceLaunch(), T.start ? "found" : "not found");
    }
    switch (T.stage) {
        case kWaiting: {
            if (now - T.up_at < S.delay_s) return;
            ue::Call c(T.start, kStartButtonClicked);
            if (T.start && c.Valid()) {
                c.Invoke();
                log::Info("launch: pressed START (the button's click event)");
                T.stage_at = now;
            } else {
                log::Warn("launch: START or its click event not found, using the title's handler");
                T.stage_at = now - 10;  // straight on to the handler next frame
            }
            T.stage = kClicked;
            g_pressed = true;
            break;
        }
        case kClicked: {
            if (now - T.stage_at < 1.0) return;
            ue::Call c(T.obj, kTitleStartHandler);
            if (c.Valid()) {
                c.Invoke();
                log::Info("launch: title screen still up, called its START handler directly");
            } else {
                log::Warn("launch: title screen has no %s", kTitleStartHandler);
            }
            T.stage = kHandler;
            T.stage_at = now;
            g_pressed = true;
            break;
        }
        case kHandler: {
            if (now - T.stage_at < 1.0) return;
            // Last resort: what a keyboard press does (focus START, Enter).
            if (T.start) {
                ue::Call f(T.start, "SetFocus");
                f.Invoke();
            }
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
            SetOpacity(T.obj, 1.0f);
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

// The Blueprint Tick functions of the game's button classes, not yet hooked.
std::vector<UObject> FindButtonTicks(const std::vector<UObject>& known) {
    std::vector<UObject> found;
    ue::objects::ForEach([&](int, UObject o) {
        if (ue::GetName(o) != "Tick") return;
        UObject outer = ue::GetOuter(o);
        if (!outer || ue::GetName(outer).rfind(kButtonClassPrefix, 0) != 0) return;
        if (ue::GetName(ue::GetClass(o)) != "Function") return;
        for (UObject k : known)
            if (k == o) return;
        found.push_back(o);
    });
    return found;
}

// Whether a title screen object exists (for the log only).
bool TitleExists() {
    bool found = false;
    ue::objects::ForEach([&](int, UObject o) {
        if (found) return;
        UObject cls = ue::GetClass(o);
        if (cls && ue::GetName(cls) == kTitleClass && ue::GetName(o).rfind("Default__", 0) != 0) found = true;
    });
    return found;
}

DWORD WINAPI WatchThread(LPVOID) {
    const double start = Now();
    double player_since = -1;
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
    log::Info("launch: following the game's widgets (%.1f s after launch)", SinceLaunch());

    std::vector<UObject> hooked;
    bool title_logged = false;
    double last_scan = -10, last_note = Now();
    while (!g_finished) {
        Sleep(100);
        const double now = Now();
        // New button classes (they load with the menus that use them).
        if (now - last_scan >= 1.0) {
            last_scan = now;
            if (hooked.size() < 8) for (UObject fn : FindButtonTicks(hooked)) {
                if (!ue::process_event::On(fn, nullptr, &OnButtonTick)) {
                    log::Warn("launch: no room to follow %s.Tick", ue::GetName(ue::GetOuter(fn)).c_str());
                    continue;
                }
                hooked.push_back(fn);
                log::Info("launch: following %s.Tick (%.1f s after launch)", ue::GetName(ue::GetOuter(fn)).c_str(),
                          SinceLaunch());
            }
            if (!title_logged && TitleExists()) {
                title_logged = true;
                log::Info("launch: a title screen object exists (%.1f s after launch, %d button ticks seen so far)",
                          SinceLaunch(), g_ticks_seen.load());
            }
        }
        if (g_pressed) {
            // The title's buttons stop ticking once it is gone.
            uint64_t last = g_last_tick_ms.load();
            if (GetTickCount64() - last > 1000) {
                log::Info("launch: title screen closed %.1f s after it came up; you're in the game",
                          (last - g_seen_at_ms.load()) / 1000.0);
                g_finished = true;
            }
            continue;
        }
        if (now - last_note > 10.0 && !g_seen_at_ms) {
            last_note = now;
            log::Info("launch: still watching for the title screen (%.0f s after launch; %zu button classes "
                      "followed, %d button ticks seen)",
                      SinceLaunch(), hooked.size(), g_ticks_seen.load());
        }
        if (game::Player()) {
            if (player_since < 0) player_since = now;
            if (now - player_since > kGiveUpS && !g_seen_at_ms) {
                log::Info("launch: no title screen came up after %.0f s of play (%d button ticks seen); "
                          "no longer watching",
                          kGiveUpS, g_ticks_seen.load());
                g_finished = true;
            }
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
