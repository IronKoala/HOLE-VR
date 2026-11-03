#include "vr/body/hand_walls.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "vr/body/ik_math.h"

namespace holevr::vr::body::hand_walls {
namespace {

using ik::Length;

struct Settings {
    bool on = true;
    double radius = 4.0;  // cm: about half a fist
} S;

struct TArrayRaw {
    const UObject* data;
    int32_t num, max;
};

// An object we hold on to between frames, with its slot in the object
// array, so a freed one is noticed before it reaches the engine.
struct Kept {
    UObject obj = 0;
    int32_t index = -1;
};
bool Alive(const Kept& k) {
    return k.obj && k.index >= 0 && mem::ReadOr<int32_t>(k.obj + ue::off::kObjIndex, -1) == k.index &&
           ue::objects::Get(k.index) == k.obj;
}

struct Side {
    bool touching = false;
    double touch_time = 0.0;  // how long this touch has lasted, s
    double deepest = 0.0;     // furthest the real hand went past the surface in this touch, cm
    std::string what;         // what the hand touched
    // A clear sweep is reused while the eyes and the hand stay put.
    bool last_clear = false;
    FVector last_from, last_to;
};
Side g_side[2];

perf::Counter g_perf{"body hands stop at walls"};
UObject g_ksl = 0, g_gps = 0;
int g_lookups = 0;
int32_t g_radius_size = -1;  // SphereTraceSingle's Radius: float (4) or double (8)
std::vector<Kept> g_attached;  // actors hanging on the player (holstered gun, mags)
double g_attached_age = 1e9;
bool g_announced = false;  // the on/off state last logged
int g_logged = 0;          // touches logged one by one this run
int g_touches = 0;         // touches since the last summary
double g_summary_age = 0;  // s since the last summary

// Actors on the player (holstered gun, pouch mags, anything the game hung on
// the character) never stop the hand. Refreshed twice a second.
void RefreshAttached(UObject player, double dt) {
    g_attached_age += dt;
    if (g_attached_age < 0.5) return;
    g_attached_age = 0;
    g_attached.clear();
    ue::Call c(player, "GetAttachedActors");
    if (!c.Valid()) return;
    // Set only the flags this engine version has (a missing name would warn
    // twice a second).
    int32_t flag = 0;
    if (c.Raw("bResetArray", &flag) && flag == 1) c.Set("bResetArray", true);
    if (c.Raw("bRecursivelyIncludeAttachedActors", &flag) && flag == 1)
        c.Set("bRecursivelyIncludeAttachedActors", true);
    if (!c.Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("OutActors", &size);
    struct Arr {
        uintptr_t data;
        int32_t num, max;
    } arr{};
    if (!raw || size < static_cast<int32_t>(sizeof(Arr))) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    for (int i = 0; i < arr.num && i < 32; ++i) {
        UObject a = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (a) g_attached.push_back({a, mem::ReadOr<int32_t>(a + ue::off::kObjIndex, -1)});
    }
}

// Sweeps the hand's ball from `from` to `to`. True with the ball's centre
// where it first touches something; false when the way is clear (or the
// sweep starts inside something: the eyes are in a wall, so there is no
// sensible place to stop the hand).
bool Sweep(UObject player, const FVector& from, const FVector& to, const std::vector<UObject>& ignore, FVector* at,
           std::string* what) {
    ue::Call t(g_ksl, "SphereTraceSingle");
    if (!t.Valid()) return false;
    if (g_radius_size < 0) {
        int32_t sz = 0;
        t.Raw("Radius", &sz);
        g_radius_size = sz;
        if (sz != 4 && sz != 8) log::Warn("hand walls: sphere trace radius is %d bytes; hands won't stop at walls", sz);
    }
    if (g_radius_size == 4)
        t.Set("Radius", static_cast<float>(S.radius));
    else if (g_radius_size == 8)
        t.Set("Radius", S.radius);
    else
        return false;
    TArrayRaw arr{ignore.data(), static_cast<int32_t>(ignore.size()), static_cast<int32_t>(ignore.size())};
    t.Set("WorldContextObject", player).Set("Start", from).Set("End", to).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return false;
    ue::Call b(g_gps, "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = b.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!b.Invoke() || !b.Get<bool>("bBlockingHit") || b.Get<bool>("bInitialOverlap")) return false;
    *at = b.Get<FVector>("Location");
    UObject actor = b.Get<UObject>("HitActor");
    *what = actor ? ue::GetName(actor) : "something";
    return true;
}

bool Near(const FVector& a, const FVector& b) { return Length(a - b) < 0.3; }

void EndTouch(int side) {
    Side& s = g_side[side];
    if (!s.touching) return;
    s.touching = false;
    if (g_logged >= 12) {
        ++g_touches;
    } else {
        ++g_logged;
        log::Info("hand walls: %s hand stopped at %s for %.1f s; the real hand went up to %.1f cm past the surface",
                  side == 0 ? "left" : "right", s.what.c_str(), s.touch_time, s.deepest);
    }
}

}  // namespace

void LoadSettings() {
    S.on = config::GetBool("hand_wall_collision", true);
    S.radius = std::clamp<double>(config::GetFloat("hand_wall_radius_cm", 4.f), 1.0, 10.0);
}

void Apply(UObject player, const hands::Pose& head, hands::HandState hs[2], const hands::HandGrab grab[2],
           double dt) {
    static bool registered = [] {
        perf::Register(&g_perf);
        return true;
    }();
    (void)registered;
    if (g_announced != S.on) {
        g_announced = S.on;
        if (S.on)
            log::Info("hand walls: on (a %.1f cm ball from the eyes to each empty hand stops at what it touches)",
                      S.radius);
        else
            log::Info("hand walls: off (hands pass through walls)");
    }
    if (!S.on || !player) {
        EndTouch(0);
        EndTouch(1);
        return;
    }
    perf::Scope timer(g_perf);
    if ((!g_ksl || !g_gps) && g_lookups < 3) {
        ++g_lookups;
        if (!g_ksl) g_ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!g_gps) g_gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        if (!g_ksl || !g_gps) log::Warn("hand walls: trace library not found; hands won't stop at walls");
    }
    if (!g_ksl || !g_gps) return;

    RefreshAttached(player, dt);
    std::vector<UObject> ignore;
    for (const Kept& k : g_attached)
        if (Alive(k)) ignore.push_back(k.obj);
    for (int i = 0; i < 2; ++i)
        if (grab[i].object) ignore.push_back(grab[i].object);

    for (int side = 0; side < 2; ++side) {
        Side& s = g_side[side];
        hands::HandState& h = hs[side];
        if (!h.valid || grab[side].kind != hands::GrabKind::kNone) {
            EndTouch(side);
            s.last_clear = false;
            continue;
        }
        const FVector from = head.location, to = h.grip.location;
        if (s.last_clear && Near(from, s.last_from) && Near(to, s.last_to)) continue;
        FVector at;
        std::string what;
        if (!Sweep(player, from, to, ignore, &at, &what)) {
            EndTouch(side);
            s.last_clear = true;
            s.last_from = from;
            s.last_to = to;
            continue;
        }
        s.last_clear = false;
        const FVector shift = at - to;
        const double depth = Length(shift);
        if (!s.touching) {
            s.touching = true;
            s.touch_time = 0;
            s.deepest = 0;
        }
        s.touch_time += dt;
        s.deepest = std::max(s.deepest, depth);
        s.what = what;
        h.grip.location = at;
        h.aim.location = h.aim.location + shift;
        h.wrist.location = h.wrist.location + shift;
    }

    g_summary_age += dt;
    if (g_summary_age >= 60.0) {
        if (g_touches > 0)
            log::Info("hand walls: %d more times a hand stopped at something in the last minute", g_touches);
        g_touches = 0;
        g_summary_age = 0;
    }
}

}  // namespace holevr::vr::body::hand_walls
