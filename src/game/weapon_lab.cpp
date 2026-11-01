#include "game/weapon_lab.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <map>
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
        log::Info("lab: F8 new magazine %s, insert %s", a ? "ok" : "failed", b ? "ok" : "failed");
    }
    if (actions & kAltInsert)
        log::Info("lab: Shift+F8 Gun_MagazineLoad -> %s", gun.CallSimple("Gun_MagazineLoad") ? "ok" : "failed");
    if (actions & kReleaseCatch)
        log::Info("lab: Ctrl+F6 slide catch release -> %s", gun.ReleaseSlideCatch() ? "ok" : "failed");
}

void UpdateSlidePull(Gun& gun, float dt) {
    bool held = g_pull_held.load();
    if (held) {
        if (!g_pulling) {
            g_pulling = true;
            g_peak_travel = 0;
            g_travel = 0;
            log::Info("lab: F6 grabbing the slide");
        }
        // About 0.6 s from closed to fully back, like a deliberate hand pull.
        g_travel = std::fmin(1.0, g_travel + dt / 0.6);
        g_peak_travel = std::fmax(g_peak_travel, g_travel);
        gun.SetSlideTravel(g_travel, AxisFor(gun.ClassName()));
        return;
    }
    if (!g_pulling) return;
    g_pulling = false;
    if (g_peak_travel >= 0.9) {
        gun.RestoreSlide();
        bool ok = gun.ChargeBolt();
        log::Info("lab: F6 released at full travel: full rack -> %s", ok ? "ok" : "failed");
    } else {
        GunState s = gun.State();
        gun.RestoreSlide();
        log::Info("lab: F6 released at %.0f%% travel: chamber check, %s in the chamber",
                  g_peak_travel * 100.0, s.chamber > 0 ? "a round is" : "nothing is");
    }
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
    UpdateSlidePull(gun, dt);

    GunState s = gun.State();
    if (s.bolt_charging || s.firing) LearnAxis(gun, s);
    if (g_trace.load() && !(s == g_last_state))
        log::Info("lab: [%.2fs] %s", g_time, s.Describe().c_str());
    g_last_state = s;
}

bool IsNoisy(const std::string& n) {
    return n.find("Tick") != std::string::npos || n.find("__UpdateFunc") != std::string::npos ||
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
