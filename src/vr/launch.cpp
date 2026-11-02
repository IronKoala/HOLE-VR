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

namespace holevr::vr::launch {
namespace {

using ue::UObject;

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t RF_BeginDestroyed = 0x8000;
constexpr uint32_t RF_FinishDestroyed = 0x10000;

// The Blueprint events a real click on START runs (seen in the 2026-11-02
// menu study logs): the button's own handler for its inner Button's
// OnClicked, which calls the title screen's handler. A key press on the title
// screen runs the title screen's handler directly.
constexpr const char* kStartButtonClicked =
    "BndEvt__UIP_Button_Primary_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
constexpr const char* kTitleStartHandler =
    "BndEvt__UIM_Title_UIP_Button_Secondary_K2Node_ComponentBoundEvent_0_OnClicked__DelegateSignature";

// If the player has been in the level this long and no title screen ever
// came up, stop looking (the object scans are not free).
constexpr double kGiveUpLookingS = 90.0;

struct Settings {
    bool auto_start = true;
    double delay_s = 0.2;
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

// --------------------------------------------------------- title screens --

enum Stage {
    kWaiting,   // not on screen yet, or waiting out the short delay
    kClicked,   // START button's click event called
    kHandler,   // title screen's START handler called directly
    kEnterKey,  // focused START and sent Enter, like a keyboard press
};

struct Title {
    UObject obj = 0;
    int index = -1;
    UObject start_button = 0;
    int start_index = -1;
    double up_at = -1;  // first seen on screen (game thread)
    double stage_at = 0;
    int stage = kWaiting;
};

std::mutex g_mutex;
std::vector<Title> g_titles;          // guarded by g_mutex
std::atomic<bool> g_job_queued{false};
std::atomic<bool> g_finished{false};  // first title screen dealt with (or none came)
UObject g_title_class = 0;            // background thread only

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

void Finish() {
    g_finished = true;
    g_titles.clear();
}

// Game thread: one step for the title screen(s) we know about. Only the first
// one that comes up is pressed; after that we are finished for this launch.
void Step() {
    g_job_queued = false;
    if (g_finished) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const double now = Now();
    for (Title& t : g_titles) {
        const bool live = Live(t.obj, t.index);
        // On screen = added to the viewport and drawn.
        const bool up = live && CallBool(t.obj, "IsInViewport") && CallBool(t.obj, "IsVisible");
        if (!up) {
            if (t.up_at >= 0) {
                log::Info("launch: title screen closed %.1f s after it came up (%.0f s after launch); "
                          "you're in the game",
                          now - t.up_at, SinceLaunch());
                Finish();
                return;
            }
            continue;
        }
        if (t.up_at < 0) {
            t.up_at = now;
            t.stage_at = now;
            // See-through until it is gone, so it never shows. Put back if we
            // give up.
            SetOpacity(t.obj, 0.0f);
            log::Info("launch: title screen %s came up %.0f s after launch; made it see-through, pressing START "
                      "(title_auto_start=0 turns this off)",
                      ue::GetName(t.obj).c_str(), SinceLaunch());
        }
        if (t.stage == kWaiting) {
            if (now - t.up_at < S.delay_s) return;
            bool btn_ok = t.start_button && Live(t.start_button, t.start_index);
            ue::Call c(btn_ok ? t.start_button : 0, kStartButtonClicked);
            if (btn_ok && c.Valid()) {
                c.Invoke();
                log::Info("launch: pressed START (the button's click event)");
                t.stage = kClicked;
                t.stage_at = now;
            } else {
                log::Warn("launch: START button or its click event not found (button %d), using the title's handler",
                          btn_ok ? 1 : 0);
                t.stage = kClicked;
                t.stage_at = now - 10;  // straight on to the handler next step
            }
        } else if (t.stage == kClicked && now - t.stage_at > 1.0) {
            ue::Call c(t.obj, kTitleStartHandler);
            if (c.Valid()) {
                c.Invoke();
                log::Info("launch: title screen still up, called its START handler directly");
            } else {
                log::Warn("launch: title screen has no %s", kTitleStartHandler);
            }
            t.stage = kHandler;
            t.stage_at = now;
        } else if (t.stage == kHandler && now - t.stage_at > 1.0) {
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
            SetOpacity(t.obj, 1.0f);
            log::Error("launch: could not get past the title screen; press START yourself "
                       "(title_auto_start=0 turns this off)");
            Finish();
        }
        return;  // one title screen at a time
    }
}

}  // namespace

void Init() {
    S.auto_start = config::GetBool("title_auto_start", true);
    S.delay_s = config::GetFloat("title_auto_start_delay", 0.2f);
    if (!S.auto_start) g_finished = true;
    log::Info("launch: %s", S.auto_start ? "START on the title screen is pressed for you the first time it comes up"
                                         : "title screen left alone (title_auto_start=0)");
}

void Poll() {
    static double s_last = 0, s_last_class = 0, s_player_since = -1;
    if (g_finished) return;
    // Check the known title screens every game frame we can, so one that
    // comes up is made see-through before it is seen.
    {
        bool pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pending = !g_titles.empty();
        }
        if (pending && !g_job_queued.exchange(true)) game::Post(&Step);
    }
    const double now = Now();
    if (now - s_last < 0.25) return;
    s_last = now;
    if (!ue::objects::Ready()) return;

    // Stop looking if the player has been in the level a long time and no
    // title screen ever came up.
    bool have_titles;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        have_titles = false;
        for (const Title& t : g_titles) have_titles |= t.up_at >= 0;
    }
    if (game::Player()) {
        if (s_player_since < 0) s_player_since = now;
        if (!have_titles && now - s_player_since > kGiveUpLookingS) {
            log::Info("launch: no title screen came up in %.0f s of play; no longer looking", kGiveUpLookingS);
            std::lock_guard<std::mutex> lock(g_mutex);
            Finish();
            return;
        }
    }

    if (!g_title_class) {
        if (now - s_last_class < 1.0) return;
        s_last_class = now;
        g_title_class = FindTitleClass();
        if (!g_title_class) return;
        log::Info("launch: found the title screen class");
    }

    // Title screen objects (a full object scan, so twice a second, and only
    // until the first one has been dealt with).
    static double s_last_scan = 0;
    if (now - s_last_scan >= 0.5) {
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
        if (g_finished) return;
        for (Title& t : added) g_titles.push_back(t);
    }
}

}  // namespace holevr::vr::launch
