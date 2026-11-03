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
#include "vr/world_menu.h"

namespace holevr::vr::body::hand_walls {
namespace {

using ik::Length;

struct Settings {
    bool on = true;
    double radius = 4.0;    // cm: about half a fist
    double probe = 14.0;    // cm: how far past the fist the fingertip probe reaches (0 = off)
} S;
constexpr double kProbeRadius = 2.0;  // cm: about a finger's width

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
    FVector last_from, last_to, last_fwd;
    // The fingertip probe found a surface ahead of the fingers.
    bool ahead = false;
    // The surface the hand last touched (fingers rest on it), kept while
    // the hand stays near it.
    bool have_plane = false;
    FVector plane_point, plane_normal;
    FVector drawn;  // where the hand is drawn this frame
};
Side g_side[2];

perf::Counter g_perf{"body hands stop at walls"};
UObject g_ksl = 0, g_gps = 0;
int g_lookups = 0;
int32_t g_radius_size = -1;  // SphereTraceSingle's Radius: float (4) or double (8)
std::vector<Kept> g_attached;  // actors hanging on the player (holstered gun, mags)
double g_attached_age = 1e9;
bool g_announced = false;  // the on/off state last logged
int g_ahead_logged = 0;    // fingertip probe hits logged this run
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
bool Sweep(UObject player, const FVector& from, const FVector& to, double radius, const std::vector<UObject>& ignore,
           FVector* at, FVector* surface, FVector* normal, std::string* what) {
    ue::Call t(g_ksl, "SphereTraceSingle");
    if (!t.Valid()) return false;
    if (g_radius_size < 0) {
        int32_t sz = 0;
        t.Raw("Radius", &sz);
        g_radius_size = sz;
        if (sz != 4 && sz != 8) log::Warn("hand walls: sphere trace radius is %d bytes; hands won't stop at walls", sz);
    }
    if (g_radius_size == 4)
        t.Set("Radius", static_cast<float>(radius));
    else if (g_radius_size == 8)
        t.Set("Radius", radius);
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
    *surface = b.Get<FVector>("ImpactPoint");
    *normal = b.Get<FVector>("ImpactNormal");
    UObject actor = b.Get<UObject>("HitActor");
    *what = actor ? ue::GetName(actor) : "something";
    return true;
}

bool Near(const FVector& a, const FVector& b) { return Length(a - b) < 0.3; }

// The fingers reach well past the fist the hand's ball is centred on. A
// thin ball swept from the fist along the fingers finds a wall or table
// ahead of them, so the fingers and palm react when the fingertips get
// there, not only once the fist does (a real hand, and the owner's
// reference, SentientArt/TheBasement, collide with the whole hand). It only
// remembers the surface; the hand isn't moved for it.
void Probe(UObject player, int side, const FVector& fist, const FVector& fwd, const std::vector<UObject>& ignore) {
    Side& s = g_side[side];
    FVector at, surface, normal;
    std::string what;
    const bool hit = S.probe > 0.0 && Sweep(player, fist, fist + fwd * S.probe, kProbeRadius, ignore, &at, &surface,
                                            &normal, &what) &&
                     ik::Length(normal) > 0.5;
    if (hit) {
        s.have_plane = true;
        s.plane_point = surface;
        s.plane_normal = normal * (1.0 / ik::Length(normal));
        if (!s.ahead && g_ahead_logged < 8) {
            ++g_ahead_logged;
            log::Info("hand walls: %s fingertips found %s %.1f cm past the fist", side == 0 ? "left" : "right",
                      what.c_str(), ik::Length(at - fist));
        }
    }
    s.ahead = hit;
}

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
    S.probe = std::clamp<double>(config::GetFloat("hand_wall_probe_cm", 14.f), 0.0, 30.0);
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
            log::Info("hand walls: on (a %.1f cm ball from the eyes to each empty hand stops at what it touches; "
                      "the fingertips feel %.0f cm past the fist)",
                      S.radius, S.probe);
        else
            log::Info("hand walls: off (hands pass through walls)");
    }
    if (!S.on || !player) {
        EndTouch(0);
        EndTouch(1);
        g_side[0].have_plane = g_side[1].have_plane = false;
        g_side[0].ahead = g_side[1].ahead = false;
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
    // The forearm menu and wrist HUD panels sit between the eyes and the
    // left hand whenever you look at your wrist.
    if (UObject holder = world_menu::PanelHolder()) ignore.push_back(holder);

    for (int side = 0; side < 2; ++side) {
        Side& s = g_side[side];
        hands::HandState& h = hs[side];
        if (!h.valid || grab[side].kind != hands::GrabKind::kNone) {
            EndTouch(side);
            s.last_clear = false;
            s.have_plane = false;
            s.ahead = false;
            continue;
        }
        const FVector from = head.location, to = h.grip.location;
        const FVector fwd = ik::Normalize(h.grip.rotation.Rotate(FVector{1, 0, 0}));
        s.drawn = to;
        // Reused while the eyes, the hand and its pointing stay put (2 deg).
        if (s.last_clear && Near(from, s.last_from) && Near(to, s.last_to) && ik::Dot(fwd, s.last_fwd) > 0.9994)
            continue;
        FVector at, surface, normal;
        std::string what;
        if (!Sweep(player, from, to, S.radius, ignore, &at, &surface, &normal, &what)) {
            EndTouch(side);
            s.last_clear = true;
            s.last_from = from;
            s.last_to = to;
            s.last_fwd = fwd;
            Probe(player, side, to, fwd, ignore);
            continue;
        }
        s.last_clear = false;
        s.ahead = false;
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
        if (ik::Length(normal) > 0.5) {
            s.have_plane = true;
            s.plane_point = surface;
            s.plane_normal = normal * (1.0 / ik::Length(normal));
        }
        s.drawn = at;
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

double Radius() { return S.radius; }

bool Touching(int side) {
    if (side < 0 || side > 1 || !S.on) return false;
    return g_side[side].touching || g_side[side].ahead;
}

bool Contact(int side, FVector* point, FVector* normal) {
    if (side < 0 || side > 1 || !S.on) return false;
    Side& s = g_side[side];
    if (!s.have_plane) return false;
    // Forget it once the hand has moved away from that spot, or is past the
    // surface's plane (round an edge or corner, where the plane isn't).
    if (ik::Length(s.drawn - s.plane_point) > 20.0 || ik::Dot(s.drawn - s.plane_point, s.plane_normal) < 0.0) {
        s.have_plane = false;
        return false;
    }
    *point = s.plane_point;
    *normal = s.plane_normal;
    return true;
}

}  // namespace holevr::vr::body::hand_walls
