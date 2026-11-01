#include "game/weapon_lab.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <vector>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/game.h"
#include "game/gun.h"
#include "ue/process_event.h"

namespace holevr::game::weapon_lab {
namespace {

std::atomic<bool> g_trace{false};
std::atomic<bool> g_pull_held{false};
std::atomic<int> g_pending{0};  // bit set of one-shot actions below

enum Action : int {
    kDumpState = 1 << 0,
    kDropMag = 1 << 1,
    kInsertMag = 1 << 2,
    kReleaseCatch = 1 << 3,
    kDetachMag = 1 << 4,
    kAltInsert = 1 << 5,
};

// Game-thread state.
std::atomic<UObject> g_gun{0};
std::atomic<UObject> g_manager{0};
GunState g_last_state;
UObject g_last_gun = 0;
double g_travel = 0.0;      // current hand-pulled slide travel 0..1
double g_peak_travel = 0.0; // furthest point of this pull
bool g_pulling = false;
double g_time = 0.0;

// Slide axis per gun class, in the slide's parent space. Learned from the
// game's own slide animations; the default suits guns modelled facing +X.
std::map<std::string, ue::Vec3d> g_axis;
ue::Vec3d AxisFor(const std::string& cls) {
    auto it = g_axis.find(cls);
    return it != g_axis.end() ? it->second : ue::Vec3d{-1, 0, 0};
}

void LearnAxis(Gun& gun, const GunState& s) {
    ue::Vec3d o = gun.SlideOrigin();
    ue::Vec3d d{s.slide_location.x - o.x, s.slide_location.y - o.y, s.slide_location.z - o.z};
    double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    double back = gun.SlideBackLength();
    // Only trust a clear, mostly-full movement.
    if (len < 0.5 || (back > 0 && len < back * 0.5)) return;
    ue::Vec3d axis{d.x / len, d.y / len, d.z / len};
    std::string cls = gun.ClassName();
    auto it = g_axis.find(cls);
    if (it == g_axis.end() || std::fabs(it->second.x - axis.x) + std::fabs(it->second.y - axis.y) +
                                      std::fabs(it->second.z - axis.z) > 0.1) {
        g_axis[cls] = axis;
        log::Info("lab: %s slide moves along (%.3f %.3f %.3f), %.2f cm seen, back length %.2f cm",
                  cls.c_str(), axis.x, axis.y, axis.z, len, back);
    }
}

void LogGunSetup(Gun& gun) {
    ue::Vec3d o = gun.SlideOrigin();
    log::Info("lab: holding %s: slide origin (%.2f %.2f %.2f) back %.2f cm, catch stop %.2f cm",
              gun.ClassName().c_str(), o.x, o.y, o.z, gun.SlideBackLength(), gun.SlideStopLength());
    log::Info("lab: state %s", gun.State().Describe().c_str());
}

void RunActions(Gun& gun, int actions) {
    if (actions & kDumpState) LogGunSetup(gun);
    if (actions & kDropMag) log::Info("lab: F7 magazine release -> %s", gun.DropMagazine() ? "ok" : "failed");
    if (actions & kDetachMag)
        log::Info("lab: Ctrl+F7 pull magazine out -> %s", gun.DetachMagazine() ? "ok" : "failed");
    if (actions & kInsertMag) {
        bool a = gun.TakeNewMagazine();
        bool b = gun.InsertMagazine();
        // The game normally reveals the new magazine from its animation.
        UObject mag = 0;
        if (ue::GetProp(gun.Object(), "MagazineObj", mag) && mag) {
            ue::Call v(mag, "SetVisibility");
            v.Set("IsVisible", true);
            v.Invoke();
            log::Info("lab: new magazine %s, made visible", GetName(mag).c_str());
        }
        log::Info("lab: F8 new magazine %s, insert %s", a ? "ok" : "failed", b ? "ok" : "failed");
    }
    if (actions & kAltInsert)
        log::Info("lab: Shift+F8 Gun_MagazineLoad -> %s", gun.CallSimple("Gun_MagazineLoad") ? "ok" : "failed");
    if (actions & kReleaseCatch)
        log::Info("lab: Ctrl+F6 slide catch release -> %s", gun.ReleaseSlideCatch() ? "ok" : "failed");
}

// Small game-thread scheduler so multi-step game actions can be spaced out.
struct Scheduled {
    double at;
    std::function<void()> fn;
};
std::vector<Scheduled> g_scheduled;
void After(double seconds, std::function<void()> fn) { g_scheduled.push_back({g_time + seconds, std::move(fn)}); }
void RunScheduled() {
    for (size_t i = 0; i < g_scheduled.size();) {
        if (g_scheduled[i].at <= g_time) {
            auto fn = std::move(g_scheduled[i].fn);
            g_scheduled.erase(g_scheduled.begin() + i);
            fn();
        } else {
            ++i;
        }
    }
}

// Which game call performs a full rack is not known yet, so each full rack
// tries the next candidate and logs what changed a second later.
int g_rack_method = 0;
constexpr int kRackMethods = 4;
const char* kRackNames[kRackMethods] = {
    "A: weapon manager ChamberAction press/release (the game's own rack)",
    "B: gun Gun_ChargeBolt pull/release",
    "C: gun ChargeBolt event pull/release",
    "D: gun SetIsBoltCharging + StartAnim_ChargeBolt",
};

void FullRack(Gun gun) {
    int m = g_rack_method;
    g_rack_method = (g_rack_method + 1) % kRackMethods;
    GunState before = gun.State();
    UObject manager = g_manager.load();
    UObject g = gun.Object();
    log::Info("lab: full rack, trying %s. Before: %s", kRackNames[m], before.Describe().c_str());
    auto call_bool = [](UObject obj, const char* fn, const char* param, bool v) {
        ue::Call c(obj, fn);
        c.Set(param, v);
        return c.Invoke();
    };
    switch (m) {
        case 0:
            call_bool(manager, "ChamberAction", "IsTriggerReleased", false);
            After(0.35, [manager, call_bool] { call_bool(manager, "ChamberAction", "IsTriggerReleased", true); });
            break;
        case 1:
            call_bool(g, "Gun_ChargeBolt", "IsTriggerReleased", false);
            After(0.35, [g, call_bool] { call_bool(g, "Gun_ChargeBolt", "IsTriggerReleased", true); });
            break;
        case 2:
            call_bool(g, "ChargeBolt", "IsTriggerReleased", false);
            After(0.35, [g, call_bool] { call_bool(g, "ChargeBolt", "IsTriggerReleased", true); });
            break;
        case 3:
            call_bool(g, "SetIsBoltCharging", "IsBoltCharging", true);
            call_bool(g, "StartAnim_ChargeBolt", "IsTriggerReleased", true);
            break;
    }
    After(1.5, [gun, m, before] {
        GunState after = Gun(gun).State();
        bool changed = !(after == before);
        log::Info("lab: full rack %c result: %s. After: %s", 'A' + m,
                  changed ? "STATE CHANGED" : "no change", after.Describe().c_str());
    });
}

void UpdateSlidePull(Gun& gun, float dt) {
    static double held_time = 0;
    bool held = g_pull_held.load();
    if (held) {
        if (!g_pulling) {
            g_pulling = true;
            g_peak_travel = 0;
            g_travel = 0;
            held_time = 0;
            log::Info("lab: F6 grabbing the slide");
        }
        held_time += dt;
        // About 0.6 s from closed to fully back, like a deliberate hand pull.
        g_travel = std::fmin(1.0, g_travel + dt / 0.6);
        g_peak_travel = std::fmax(g_peak_travel, g_travel);
        gun.SetSlideTravel(g_travel, AxisFor(gun.ClassName()));
        return;
    }
    if (!g_pulling) return;
    g_pulling = false;
    // Read back where the slide part actually is, to confirm the write took.
    GunState s = gun.State();
    ue::Vec3d o = gun.SlideOrigin();
    log::Info("lab: slide part at (%.2f %.2f %.2f), origin (%.2f %.2f %.2f)", s.slide_location.x,
              s.slide_location.y, s.slide_location.z, o.x, o.y, o.z);
    if (g_peak_travel >= 0.9) {
        gun.RestoreSlide();
        FullRack(gun);
        return;
    }
    if (held_time < 0.25) {
        // A tap: do a whole chamber check for the player. Pull to half way,
        // hold for a second so the round is visible, then let it go.
        ue::Vec3d axis = AxisFor(gun.ClassName());
        Gun g = gun;
        for (int i = 1; i <= 10; ++i) After(0.02 * i, [g, axis, i] { Gun(g).SetSlideTravel(0.05 * i, axis); });
        for (int i = 0; i < 50; ++i) After(0.2 + 0.02 * i, [g, axis] { Gun(g).SetSlideTravel(0.5, axis); });
        After(1.25, [g] { Gun(g).RestoreSlide(); });
    } else {
        gun.RestoreSlide();
    }
    log::Info("lab: F6 chamber check (%.0f%% travel): %s in the chamber", (held_time < 0.25 ? 50.0 : g_peak_travel * 100.0),
              s.chamber > 0 ? "a round is" : "nothing is");
}

void OnTick(UObject player, float dt) {
    g_time += dt;
    Gun gun = Gun::Current(player);
    UObject manager = 0;
    ue::GetProp(player, "WeaponManager", manager);
    g_manager.store(manager);
    g_gun.store(gun.Object());

    int actions = g_pending.exchange(0);
    if (!gun.Valid()) {
        if (actions) log::Info("lab: not holding a gun");
        g_last_gun = 0;
        return;
    }
    if (gun.Object() != g_last_gun) {
        g_last_gun = gun.Object();
        g_last_state = GunState();
        LogGunSetup(gun);
    }
    RunActions(gun, actions);
    RunScheduled();
    UpdateSlidePull(gun, dt);

    GunState s = gun.State();
    if (s.bolt_charging || s.firing) LearnAxis(gun, s);
    if (g_trace.load() && !(s == g_last_state))
        log::Info("lab: [%.2fs] %s", g_time, s.Describe().c_str());
    g_last_state = s;
}

bool IsNoisy(const std::string& n) {
    return n.find("Tick") != std::string::npos || n.rfind("InpAxisEvt", 0) == 0 ||
           n.find("FinishedFunc") != std::string::npos || n.find("__UpdateFunc") != std::string::npos ||
           n.rfind("Get", 0) == 0 || n.rfind("BPI_Get", 0) == 0 || n.rfind("Update", 0) == 0 ||
           n.find("Overlap") != std::string::npos;
}

// Logs the game's own weapon events (reload notifies, charge, fire) so the VR
// version can call the same steps in the same order.
void OnAnyEvent(UObject self, UObject function, void*) {
    if (!g_trace.load(std::memory_order_relaxed)) return;
    UObject gun = g_gun.load(std::memory_order_relaxed);
    UObject manager = g_manager.load(std::memory_order_relaxed);
    UObject player = Player();
    if (!self || (self != gun && self != manager && self != player)) return;
    std::string name = GetName(function);
    if (IsNoisy(name)) return;
    const char* who = self == gun ? "gun" : self == manager ? "weapon manager" : "player";
    log::Info("lab: [%.2fs] %s.%s", g_time, who, name.c_str());
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

struct Edge {
    bool was = false;
    bool Pressed(bool now) {
        bool p = now && !was;
        was = now;
        return p;
    }
};
Edge e5, e7, e8, e6c;

}  // namespace

void Init() {
    game::AddTickListener(&OnTick);
    ue::process_event::OnAny(&OnAnyEvent);
    if (config::GetBool("lab_trace_on_start", false)) g_trace = true;
    log::Info("lab: weapon lab ready (F5 trace, hold F6 slide, F7 drop mag, F8 insert mag)");
}

void PollHotkeys() {
    // Only react while the game window has focus.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        g_pull_held = false;
        return;
    }
    bool ctrl = Down(VK_CONTROL);
    if (e5.Pressed(Down(VK_F5))) {
        bool on = !g_trace.load();
        g_trace = on;
        g_pending |= kDumpState;
        log::Info("lab: tracing %s", on ? "ON" : "OFF");
    }
    g_pull_held = Down(VK_F6) && !ctrl;
    if (e6c.Pressed(Down(VK_F6) && ctrl)) g_pending |= kReleaseCatch;
    if (e7.Pressed(Down(VK_F7))) g_pending |= ctrl ? kDetachMag : kDropMag;
    bool shift = Down(VK_SHIFT);
    if (e8.Pressed(Down(VK_F8) && !ctrl)) g_pending |= shift ? kAltInsert : kInsertMag;
}

}  // namespace holevr::game::weapon_lab
