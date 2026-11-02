#include "vr/launch.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "ue/call.h"
#include "ue/objects.h"
#include "vr/openxr.h"

namespace holevr::vr::launch {
namespace {

using ue::UObject;

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;

// The Blueprint events a real click on START runs (seen in the 2026-11-02
// menu study log): first the button's own handler for its inner Button's
// OnClicked, which then calls the title screen's handler.
constexpr const char* kStartButtonClicked =
    "BndEvt__UIP_Button_Primary_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
constexpr const char* kTitleStartHandler =
    "BndEvt__UIM_Title_UIP_Button_Secondary_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature";

struct Settings {
    bool auto_start = true;
    double delay_s = 0.5;
} S;

double Now() { return GetTickCount64() / 1000.0; }

// Seconds since the game process started (for "VR came on N s after launch").
double SinceLaunch() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c{{created.dwLowDateTime, created.dwHighDateTime}};
    ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
    return (n.QuadPart - c.QuadPart) / 1e7;
}

int IndexOf(UObject obj) { return mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1); }

bool Live(UObject obj, int index) {
    if (!obj || index < 0 || ue::objects::Get(index) != obj) return false;
    uint32_t flags = mem::ReadOr<uint32_t>(obj + ue::off::kObjFlags, RF_BeginDestroyed);
    return !(flags & (RF_BeginDestroyed | RF_FinishDestroyed | RF_ClassDefaultObject));
}

bool CallBool(UObject obj, const char* fn) {
    ue::Call c(obj, fn);
    return c.Invoke() && c.Result<bool>();
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

// --------------------------------------------------------- title screens --

enum Stage {
    kWaiting,       // not on screen yet, or waiting out the delay
    kClicked,       // START button's click event called
    kHandler,       // title screen's START handler called directly
    kEnterKey,      // focused START and sent Enter, like a keyboard press
    kDone,          // gone (or we gave up)
};

struct Title {
    UObject obj = 0;
    int index = -1;
    UObject start_button = 0;
    int start_index = -1;
    double up_at = -1;     // first seen on screen (game thread)
    double stage_at = 0;
    int stage = kWaiting;
    bool failed = false;        // we gave up on it (left for the keyboard)
    bool gone_since = false;    // off screen at some point since kDone
};

std::mutex g_mutex;
std::vector<Title> g_titles;      // guarded by g_mutex
std::atomic<bool> g_job_queued{false};
UObject g_title_class = 0;        // background thread only

// The class object of the title screen widget (a Blueprint widget class).
UObject FindTitleClass() {
    UObject found = 0;
    ue::objects::ForEach([&](int, UObject o) {
        if (found) return;
        if (ue::GetName(o) != "UIM_Title_C") return;
        if (ue::GetName(ue::GetClass(o)) != "WidgetBlueprintGeneratedClass") return;
        found = o;
    });
    return found;
}

// START inside a title screen: its widget variable, else the widget named
// UIP_Btn_START in the title's widget tree.
UObject FindStartButton(UObject title) {
    UObject btn = 0;
    if (ue::GetProp(title, "UIP_Btn_START", btn) && btn) return btn;
    ue::objects::ForEach([&](int, UObject o) {
        if (btn) return;
        if (ue::GetName(o) != "UIP_Btn_START") return;
        UObject tree = ue::GetOuter(o);
        if (tree && ue::GetOuter(tree) == title) btn = o;
    });
    return btn;
}

// Game thread: one step for each title screen we know about.
void Step() {
    g_job_queued = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const double now = Now();
    for (Title& t : g_titles) {
        if (t.stage == kDone) {
            // The game may put the same title screen up again later (Return
            // to Title): then it is a new appearance to deal with.
            if (t.failed || !Live(t.obj, t.index)) continue;
            bool up = CallBool(t.obj, "IsInViewport") && CallBool(t.obj, "IsVisible");
            if (!up) {
                t.gone_since = true;
                continue;
            }
            if (!t.gone_since) continue;
            log::Info("launch: title screen %s is back", ue::GetName(t.obj).c_str());
            t.stage = kWaiting;
            t.up_at = -1;
            t.gone_since = false;
        }
        if (!Live(t.obj, t.index)) {
            if (t.stage != kWaiting)
                log::Info("launch: title screen gone %.1f s after pressing START", now - t.stage_at);
            t.stage = kDone;
            continue;
        }
        // On screen = added to the viewport and drawn. A title screen the
        // game has finished with is neither, so it is never pressed.
        bool up = CallBool(t.obj, "IsInViewport") && CallBool(t.obj, "IsVisible");
        if (!up) {
            if (t.stage != kWaiting) {
                log::Info("launch: title screen gone %.1f s after pressing START; you're in the game",
                          now - t.stage_at);
                t.stage = kDone;
            }
            continue;
        }
        if (t.up_at < 0) {
            t.up_at = now;
            // Fully transparent until it is gone, so the flat title menu never
            // shows in the headset as a box. Put back if we give up.
            ue::Call o(t.obj, "SetRenderOpacity");
            o.Set("InOpacity", 0.0f);
            bool hidden = o.Invoke();
            log::Info("launch: title screen %s is up and the headset is on (%.0f s after launch); "
                      "%s, pressing START in %.1f s (title_auto_start=0 turns this off)",
                      ue::GetName(t.obj).c_str(), SinceLaunch(), hidden ? "made it see-through" : "could not hide it",
                      S.delay_s);
        }
        if (t.stage == kWaiting) {
            if (now - t.up_at < S.delay_s) continue;
            bool btn_ok = t.start_button && Live(t.start_button, t.start_index);
            ue::Call c(btn_ok ? t.start_button : 0, kStartButtonClicked);
            if (btn_ok && c.Valid()) {
                c.Invoke();
                log::Info("launch: pressed START (the button's click event)");
                t.stage = kClicked;
            } else {
                log::Warn("launch: START button or its click event not found (button %d), using the title's handler",
                          btn_ok ? 1 : 0);
                t.stage = kClicked;  // fall through to the handler on this step
                t.stage_at = now - 10;
                continue;
            }
            t.stage_at = now;
        } else if (t.stage == kClicked && now - t.stage_at > 1.5) {
            ue::Call c(t.obj, kTitleStartHandler);
            if (c.Valid()) {
                c.Invoke();
                log::Info("launch: title screen still up, called its START handler directly");
            } else {
                log::Warn("launch: title screen has no %s", kTitleStartHandler);
            }
            t.stage = kHandler;
            t.stage_at = now;
        } else if (t.stage == kHandler && now - t.stage_at > 1.5) {
            // Last resort: what a keyboard press does (focus START, Enter).
            bool btn_ok = t.start_button && Live(t.start_button, t.start_index);
            if (btn_ok) {
                ue::Call f(t.start_button, "SetFocus");
                f.Invoke();
            }
            HWND w = GameWindow();
            if (w) {
                PostMessageW(w, WM_KEYDOWN, VK_RETURN, 1);
                PostMessageW(w, WM_KEYUP, VK_RETURN, (1u << 31) | (1u << 30) | 1);
            }
            log::Info("launch: title screen still up, sent Enter to START (button %d, window %d)", btn_ok ? 1 : 0,
                      w ? 1 : 0);
            t.stage = kEnterKey;
            t.stage_at = now;
        } else if (t.stage == kEnterKey && now - t.stage_at > 2.0) {
            ue::Call o(t.obj, "SetRenderOpacity");
            o.Set("InOpacity", 1.0f);
            o.Invoke();
            log::Error("launch: could not get past the title screen; press Enter on the keyboard "
                       "(title_auto_start=0 turns this off)");
            t.stage = kDone;
            t.failed = true;
        }
    }
}

}  // namespace

void Init() {
    S.auto_start = config::GetBool("title_auto_start", true);
    S.delay_s = config::GetFloat("title_auto_start_delay", 0.5f);
    log::Info("launch: %s", S.auto_start ? "START on the title screen is pressed for you once the headset is on"
                                         : "title screen left alone (title_auto_start=0)");
}

void Poll() {
    static double s_last = 0, s_last_class = 0;
    static bool s_told_visible = false;
    const double now = Now();
    if (now - s_last < 0.25) return;
    s_last = now;

    const bool visible = xr::SessionVisible();
    if (visible && !s_told_visible) {
        s_told_visible = true;
        log::Info("launch: the headset is showing the game, %.0f s after launch", SinceLaunch());
    }
    if (!S.auto_start || !ue::objects::Ready()) return;

    if (!g_title_class) {
        if (now - s_last_class < 2.0) return;
        s_last_class = now;
        g_title_class = FindTitleClass();
        if (!g_title_class) return;
        log::Info("launch: found the title screen class");
    }

    // New title screens (one each launch, and again after Return to Title).
    // A full object scan, so only while the headset is on: twice a second
    // until the first title screen is dealt with, then every 1.5 s.
    static double s_last_scan = 0;
    static bool s_first_done = false;
    if (!s_first_done) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Title& t : g_titles) s_first_done |= t.stage == kDone && t.up_at >= 0;
    }
    if (visible && now - s_last_scan >= (s_first_done ? 1.5 : 0.5)) {
        s_last_scan = now;
        std::vector<std::pair<UObject, int>> found;
        ue::objects::ForEach([&](int index, UObject o) {
            if (ue::GetClass(o) != g_title_class) return;
            if (!Live(o, index)) return;
            found.push_back({o, index});
        });
        std::vector<Title> added;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& [o, index] : found) {
                bool known = false;
                for (const Title& t : g_titles) known |= t.obj == o && t.index == index;
                if (!known) added.push_back({o, index});
            }
        }
        for (Title& t : added) {  // the button search can scan, so outside the lock
            t.start_button = FindStartButton(t.obj);
            t.start_index = t.start_button ? IndexOf(t.start_button) : -1;
            log::Info("launch: title screen %s found (START button %s)", ue::GetName(t.obj).c_str(),
                      t.start_button ? "found" : "not found");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Title& t : added) g_titles.push_back(t);
    }

    // Press only while the headset is showing the game, so you start the
    // level in VR (not before SteamVR is up).
    if (!visible) return;
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Title& t : g_titles) pending |= !t.failed;
    }
    if (pending && !g_job_queued.exchange(true)) game::Post(&Step);
}

}  // namespace holevr::vr::launch
