#include "vr/workbench.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "game/game.h"
#include "game/gun.h"
#include "game/weapon_custom.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "vr/hands.h"
#include "vr/input.h"
#include "vr/math.h"

namespace holevr::vr::workbench {
namespace {

using ue::FName;
using ue::UObject;
namespace custom = game::custom;

constexpr const char* kBenchClass =
    "BlueprintGeneratedClass /Game/GamePlay/LevelComponents/LCBP_WeaponCustom.LCBP_WeaponCustom_C";
// The hub facility the bench belongs to (LCBP_HomeFacilityBase_C): locked
// behind a yellow sign until bought.
constexpr const char* kFacilityClass = "BlueprintGeneratedClass /Game/GamePlay/LevelComponents/HF_WPSWC.HF_WPSWC_C";
constexpr uint8_t kFacilityUnlocked = 2;  // FacilityState: 0 empty, 1 yellow sign, 2 unlocked (CBP_N_ children)

// ---------------------------------------------------------------- settings
struct Settings {
    bool study = true;
    bool enabled = true;
    bool tables_before_unlock = true;
    double grab_cm = 3.0;
    double snap_cm = 5.0;
    double walk_away_cm = 300.0;
    double bench_reach_cm = 250.0;  // unlocked bench: let go this close to it to lay the gun down
    double rest_cm = 1.7;           // grab point above the table when the pistol lies on its side
} S;

// ---------------------------------------------------------------- engine helpers
struct alignas(16) Xf {
    FQuat rotation;
    FVector translation;
    double pad0 = 0;
    FVector scale{1, 1, 1};
    double pad1 = 0;
};
static_assert(sizeof(Xf) == 0x60, "FTransform layout");

struct Arr {
    uintptr_t data;
    int32_t num, max;
};

double Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
FVector Norm(const FVector& v) {
    const double l = Len(v);
    return l > 1e-6 ? v * (1.0 / l) : FVector{1, 0, 0};
}
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
double Flat(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// Rotation whose X/Y/Z axes are the given orthonormal vectors.
FQuat FromAxes(const FVector& x, const FVector& y, const FVector& z) {
    const double m00 = x.X, m11 = y.Y, m22 = z.Z, tr = m00 + m11 + m22;
    FQuat q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q.W = 0.25 * s;
        q.X = (y.Z - z.Y) / s;
        q.Y = (z.X - x.Z) / s;
        q.Z = (x.Y - y.X) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q.W = (y.Z - z.Y) / s;
        q.X = 0.25 * s;
        q.Y = (y.X + x.Y) / s;
        q.Z = (z.X + x.Z) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q.W = (z.X - x.Z) / s;
        q.X = (y.X + x.Y) / s;
        q.Y = 0.25 * s;
        q.Z = (z.Y + y.Z) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q.W = (x.Y - y.X) / s;
        q.X = (z.X + x.Z) / s;
        q.Y = (z.Y + y.Z) / s;
        q.Z = 0.25 * s;
    }
    return q;
}

UObject Statics() {
    static UObject s = 0;
    if (!s) s = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    return s;
}
UObject Ksl() {
    static UObject s = 0;
    if (!s) s = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    return s;
}

FVector ActorLocation(UObject a) {
    ue::Call c(a, "K2_GetActorLocation");
    c.Invoke();
    return c.Result<FVector>();
}
FQuat ActorRotation(UObject a) {
    ue::Call c(a, "K2_GetActorRotation");
    c.Invoke();
    return ToQuat(c.Result<FRotator>());
}
void SetActorPose(UObject a, const FVector& loc, const FQuat& rot) {
    ue::Call c(a, "K2_SetActorLocationAndRotation");
    c.Set("NewLocation", loc).Set("NewRotation", ToRotator(rot)).Set("bSweep", false).Set("bTeleport", true);
    c.Invoke();
}
Xf ComponentToWorld(UObject comp) {
    ue::Call c(comp, "K2_GetComponentToWorld");
    if (!c.Invoke()) return Xf{};
    return c.Result<Xf>();
}
Xf RelativeTransform(UObject comp) {
    ue::Call c(comp, "GetRelativeTransform");
    if (!c.Invoke()) return Xf{};
    return c.Result<Xf>();
}
void SetHidden(UObject a, bool hidden) {
    ue::Call c(a, "SetActorHiddenInGame");
    c.Set("bNewHidden", hidden);
    c.Invoke();
}
bool LiveObject(UObject obj) {
    if (!obj) return false;
    const int32_t idx = mem::ReadOr<int32_t>(obj + ue::off::kObjIndex, -1);
    return idx >= 0 && ue::objects::Get(idx) == obj && ue::GetClass(obj) != 0;
}
void DestroyActor(UObject a) {
    if (!LiveObject(a)) return;
    ue::Call c(a, "K2_DestroyActor");
    c.Invoke();
}

UObject SpawnActor(UObject world_context, UObject cls, const FVector& loc, const FQuat& rot) {
    if (!Statics() || !cls) return 0;
    Xf t;
    t.rotation = rot;
    t.translation = loc;
    ue::Call begin(Statics(), "BeginDeferredActorSpawnFromClass");
    const uint8_t always_spawn = 1;  // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    begin.Set("WorldContextObject", world_context).Set("ActorClass", cls).Set("SpawnTransform", t)
        .Set("CollisionHandlingOverride", always_spawn);
    if (!begin.Invoke()) return 0;
    UObject actor = begin.Result<UObject>();
    if (!actor) return 0;
    ue::Call finish(Statics(), "FinishSpawningActor");
    finish.Set("Actor", actor).Set("SpawnTransform", t);
    finish.Invoke();
    UObject done = finish.Result<UObject>();
    if (!done) done = actor;
    // Loose parts are only looked at and carried: no collision (hands, the
    // gun and the table traces pass through them).
    ue::Call col(done, "SetActorEnableCollision");
    col.Set("bNewActorEnableCollision", false);
    col.Invoke();
    return done;
}

// Straight down from `from` to `to`: the first thing hit, ignoring `ignore`.
bool TraceDown(UObject world_context, const FVector& from, const FVector& to, const std::vector<UObject>& ignore,
               FVector* at, FVector* normal, std::string* what) {
    if (!Ksl() || !Statics()) return false;
    Arr arr{ignore.empty() ? 0 : reinterpret_cast<uintptr_t>(ignore.data()), static_cast<int32_t>(ignore.size()),
            static_cast<int32_t>(ignore.size())};
    ue::Call t(Ksl(), "LineTraceSingle");
    t.Set("WorldContextObject", world_context).Set("Start", from).Set("End", to).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("bIgnoreSelf", true);
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t hit_size = 0;
    uint8_t* hit = t.Raw("OutHit", &hit_size);
    if (!hit) return false;
    ue::Call br(Statics(), "BreakHitResult");
    int32_t in_size = 0;
    uint8_t* in = br.Raw("Hit", &in_size);
    if (!in || in_size != hit_size) return false;
    std::memcpy(in, hit, static_cast<size_t>(hit_size));
    if (!br.Invoke() || !br.Get<bool>("bBlockingHit")) return false;
    *at = br.Get<FVector>("ImpactPoint");
    *normal = br.Get<FVector>("ImpactNormal");
    UObject actor = br.Get<UObject>("HitActor");
    *what = actor ? ue::GetName(actor) : "something";
    return true;
}

// Actors hanging on `actor` (a gun's magazine and parts), for trace ignore lists.
void AddAttached(UObject actor, std::vector<UObject>* out) {
    ue::Call c(actor, "GetAttachedActors");
    if (!c.Valid()) return;
    int32_t flag = 0;
    if (c.Raw("bResetArray", &flag) && flag == 1) c.Set("bResetArray", true);
    if (c.Raw("bRecursivelyIncludeAttachedActors", &flag) && flag == 1)
        c.Set("bRecursivelyIncludeAttachedActors", true);
    if (!c.Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("OutActors", &size);
    Arr arr{};
    if (!raw || size < 16) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    for (int32_t i = 0; i < arr.num && i < 32; ++i)
        if (UObject a = mem::ReadOr<uintptr_t>(arr.data + i * 8ull)) out->push_back(a);
}

// An actor's visible static meshes as one box in the actor's own space.
struct Box {
    FVector lo{-2, -2, -2}, hi{2, 2, 2};
    bool measured = false;
    FVector Center() const { return (lo + hi) * 0.5; }
};
Box LocalBox(UObject actor) {
    Box out;
    static UObject smc_class = 0;
    if (!smc_class) smc_class = ue::FindObject("Class /Script/Engine.StaticMeshComponent");
    if (!smc_class) return out;
    ue::Call list(actor, "K2_GetComponentsByClass");
    list.Set("ComponentClass", smc_class);
    int32_t size = 0;
    const uint8_t* raw = list.Invoke() ? list.Raw("ReturnValue", &size) : nullptr;
    Arr arr{};
    if (raw && size >= 16) std::memcpy(&arr, raw, sizeof(Arr));
    const FVector al = ActorLocation(actor);
    const FQuat inv = ActorRotation(actor).Inverse();
    for (int32_t i = 0; i < arr.num && i < 32; ++i) {
        UObject c = mem::ReadOr<uintptr_t>(arr.data + i * 8ull);
        if (!c) continue;
        ue::Call vis(c, "IsVisible");
        if (vis.Invoke() && !vis.Result<bool>()) continue;
        UObject sm = 0;
        if (!ue::GetProp(c, "StaticMesh", sm) || !sm) continue;
        ue::Call bb(sm, "GetBoundingBox");
        int32_t bsz = 0;
        const uint8_t* b = bb.Invoke() ? bb.Raw("ReturnValue", &bsz) : nullptr;
        if (!b || bsz < 48) continue;
        double v[6];
        std::memcpy(v, b, sizeof(v));
        // Skip empty meshes and big helper meshes (a flashlight's beam cone).
        const double dx = v[3] - v[0], dy = v[4] - v[1], dz = v[5] - v[2];
        if (dx < 0.05 && dy < 0.05 && dz < 0.05) continue;
        if (dx > 40 || dy > 40 || dz > 40) continue;
        const Xf w = ComponentToWorld(c);
        for (int k = 0; k < 8; ++k) {
            const FVector corner{k & 1 ? v[3] : v[0], k & 2 ? v[4] : v[1], k & 4 ? v[5] : v[2]};
            const FVector scaled{corner.X * w.scale.X, corner.Y * w.scale.Y, corner.Z * w.scale.Z};
            const FVector l = inv.Rotate(w.translation + w.rotation.Rotate(scaled) - al);
            if (!out.measured) {
                out.lo = out.hi = l;
                out.measured = true;
            }
            out.lo = {std::min(out.lo.X, l.X), std::min(out.lo.Y, l.Y), std::min(out.lo.Z, l.Z)};
            out.hi = {std::max(out.hi.X, l.X), std::max(out.hi.Y, l.Y), std::max(out.hi.Z, l.Z)};
        }
    }
    return out;
}

// Distance from a world point to an actor's box (0 inside).
double DistToBox(const FVector& p, const FVector& loc, const FQuat& rot, const Box& b) {
    const FVector l = rot.Inverse().Rotate(p - loc);
    const FVector c{std::clamp(l.X, b.lo.X, b.hi.X), std::clamp(l.Y, b.lo.Y, b.hi.Y), std::clamp(l.Z, b.lo.Z, b.hi.Z)};
    return Len(l - c);
}

// ---------------------------------------------------------------- level state
struct Level {
    UObject player = 0;
    double age = 0;
    bool searched = false;
    UObject bench = 0;
    UObject facility = 0;
    int facility_state = -1;
    bool unlocked = false;
    FVector bench_loc;
    double next_state_check = 0;
    bool studied = false;
    double next_near_check = 0;
    bool logged_near = false;
    // Game data, read when a session starts.
    bool have_db = false;
    std::vector<custom::PartRow> db;
} L;

std::atomic<int> g_search_state{0};  // 0 idle, 1 running, 2 done
std::atomic<UObject> g_found_bench{0}, g_found_facility{0}, g_search_for{0};
// Hub classes: freed when you leave the hub and loaded again at a new address
// when you come back, so kept as ObjectRefs (only the search thread uses them).
ue::ObjectRef g_bench_class(kBenchClass, 2.0), g_facility_class(kFacilityClass, 2.0);

// ---------------------------------------------------------------- workbench state
struct Park {
    bool on = false;
    UObject gun = 0;
    FVector spot;   // where a hand grabs it
    FQuat frame;    // gun frame in the world
    double surface_z = 0;
} P;

// A part lying on the table: our own actor of the part's class.
struct Loose {
    std::string row;
    FName name;
    UObject actor = 0;
    Box box;
    FQuat rest_rot;
    bool have_rest = false;
};

// A part in a hand.
struct Held {
    bool active = false;
    std::string row;
    FName name;
    UObject actor = 0;          // our actor, following the hand
    Box box;
    bool from_gun = false;      // taken off the gun: the gun's own part is hidden until it is settled
    UObject gun_part = 0;
    int from_slot = -1;         // slot index it came from
    FVector rel_loc;            // actor pose in the hand's grip frame
    FQuat rel_rot;
    int snapped = -1;           // slot index it sits on now (shown on the gun), -1 none
    std::string snapped_name;
};

struct Session {
    bool on = false;
    UObject gun = 0;
    FName weapon_row;
    std::string weapon;
    FVector anchor;     // where the gun was laid down
    FVector along;      // barrel direction, flat
    FVector toward;     // from the gun toward the player, flat
    double surface_z = 0;
    std::vector<FName> owned;
    bool dirty = false; // the table needs laying out again
} T;

std::vector<Loose> g_table;
Held g_held[2];

// What a hand would grab (ClaimsGrip decides it, the tick acts on it).
struct Candidate {
    int kind = 0;  // 0 none, 1 loose part (index into g_table), 2 part on the gun
    int index = -1;
    UObject actor = 0;
    std::string row;
    int slot = -1;
    double dist = 1e9;
};
struct Claim {
    double time = -1;
    Candidate c;
} g_claim[2];
bool g_hover[2] = {false, false};
std::map<UObject, Box> g_box_cache;

const char* HandName(int h) { return h == 0 ? "left" : "right"; }

const custom::PartRow* Part(const std::string& row) {
    for (const auto& r : L.db)
        if (r.name == row) return &r;
    return nullptr;
}
bool IsMagSlot(const std::string& id) { return id.compare(0, 3, "MAG") == 0; }
bool Owned(const FName& n) {
    for (const auto& o : T.owned)
        if (custom::SameName(o, n)) return true;
    return false;
}
UObject CurrentGun() { return L.player ? game::Gun::Current(L.player).Object() : 0; }

// Top-level slots of the gun that take attachments (not magazine or ammo).
std::vector<custom::Slot> PartSlots(UObject gun) {
    std::vector<custom::Slot> out;
    for (auto& s : custom::Slots(gun))
        if (!s.sub && !IsMagSlot(s.slot_id)) out.push_back(s);
    return out;
}
const custom::Slot* SlotByIndex(const std::vector<custom::Slot>& slots, int index) {
    for (const auto& s : slots)
        if (s.index == index) return &s;
    return nullptr;
}

// Where a part actor sits when it is fitted to `slot`: the game attaches the
// part to the slot point, then offsets it by the inverse of its AttachPoint
// (Init_GenerateWeaponPartsByCustomData). Sub slots (a compensator on the
// slide) take the part when its AttachSubID matches.
bool FittedPose(UObject gun, const custom::Slot& slot, const custom::PartRow& part, UObject part_actor, FVector* loc,
                FQuat* rot, FVector* slot_loc) {
    UObject target = slot.component;
    if (!part.attach_sub_id.empty() && part.attach_sub_id != "None") {
        for (const auto& s : custom::Slots(gun))
            if (s.sub && s.index == slot.index && s.slot_id == part.attach_sub_id) target = s.component;
    }
    UObject ap = 0;
    if (!target || !ue::GetProp(part_actor, "AttachPoint", ap) || !ap) return false;
    const Xf sw = ComponentToWorld(target);
    const Xf rel = RelativeTransform(ap);
    *rot = sw.rotation * rel.rotation.Inverse();
    *loc = sw.translation - rot->Rotate(rel.translation);
    *slot_loc = sw.translation;
    return true;
}

// ---------------------------------------------------------------- the table
void DestroyLoose(Loose& l) {
    DestroyActor(l.actor);
    l.actor = 0;
}

void ReturnHeld(int h, const char* why) {
    Held& hd = g_held[h];
    if (!hd.active) return;
    if (hd.from_gun) {
        if (LiveObject(hd.gun_part)) SetHidden(hd.gun_part, false);
        DestroyActor(hd.actor);
    } else if (T.on && LiveObject(hd.actor)) {
        Loose l;
        l.row = hd.row;
        l.name = hd.name;
        l.actor = hd.actor;
        l.box = hd.box;
        g_table.push_back(l);
        T.dirty = true;
    } else {
        DestroyActor(hd.actor);
    }
    if (why) log::Info("workbench: %s back %s", hd.row.c_str(), why);
    hd = Held{};
}

void EndSession(const char* why, bool level_gone = false) {
    if (!T.on && !P.on && g_table.empty() && !g_held[0].active && !g_held[1].active) return;
    for (int h = 0; h < 2; ++h) {
        if (level_gone) g_held[h] = Held{};
        else ReturnHeld(h, nullptr);
    }
    if (!level_gone)
        for (auto& l : g_table) DestroyLoose(l);
    g_table.clear();
    g_box_cache.clear();
    if (P.on) log::Info("workbench: the pistol goes back to the holster (%s)", why);
    P = Park{};
    if (T.on) log::Info("workbench: table cleared (%s)", why);
    T = Session{};
}

// Owned attachments that fit this gun and are not on it, in a row beside it.
void LayOutTable() {
    T.dirty = false;
    UObject gun = T.gun;
    if (!T.on || !LiveObject(gun)) return;
    std::vector<custom::SavedPart> saved;
    UObject gi = custom::GameInstance(L.player);
    custom::Saved(gi, T.weapon_row, &saved);
    const std::vector<custom::Slot> slots = PartSlots(gun);
    std::vector<const custom::PartRow*> want;
    for (const auto& r : L.db) {
        if (!Owned(r.row)) continue;
        bool fits = false;
        for (const auto& s : slots)
            if (s.slot_id == r.attach_id) fits = true;
        if (!fits) continue;
        bool fitted = false;
        for (const auto& sp : saved)
            if (custom::SameName(sp.row, r.row)) fitted = true;
        for (const auto& hd : g_held)
            if (hd.active && hd.row == r.name && !hd.from_gun) fitted = true;  // in a hand
        if (!fitted) want.push_back(&r);
    }
    // Drop what should not lie here any more, then add what is missing.
    for (auto& l : g_table) {
        bool keep = LiveObject(l.actor);
        if (keep) {
            keep = false;
            for (auto* w : want)
                if (w->name == l.row) keep = true;
        }
        if (!keep) DestroyLoose(l);
    }
    g_table.erase(std::remove_if(g_table.begin(), g_table.end(), [](const Loose& l) { return !l.actor; }),
                  g_table.end());
    for (auto* w : want) {
        bool have = false;
        for (auto& l : g_table)
            if (l.row == w->name) have = true;
        if (have) continue;
        UObject cls = custom::PartClass(*w);
        UObject a = cls ? SpawnActor(L.player, cls, T.anchor + FVector{0, 0, 30}, FQuat{}) : 0;
        if (!a) {
            log::Warn("workbench: could not make %s to lay on the table (class %s)", w->name.c_str(),
                      cls ? "found" : "not found");
            continue;
        }
        Loose l;
        l.row = w->name;
        l.name = w->row;
        l.actor = a;
        l.box = LocalBox(a);
        g_table.push_back(l);
    }
    // Lay them out: a row 14 cm toward the player, 9 cm apart, each turned
    // the way it sits on the gun lying there.
    std::sort(g_table.begin(), g_table.end(), [](const Loose& a, const Loose& b) { return a.row < b.row; });
    const int n = static_cast<int>(g_table.size());
    std::string listed;
    std::vector<UObject> ignore{L.player, gun};
    AddAttached(gun, &ignore);
    for (auto& l : g_table) ignore.push_back(l.actor);
    for (int i = 0; i < n; ++i) {
        Loose& l = g_table[i];
        const custom::PartRow* row = Part(l.row);
        FQuat rot = l.have_rest ? l.rest_rot : FromAxes(T.along, Cross(FVector{0, 0, 1}, T.along), FVector{0, 0, 1});
        if (row && P.on) {
            for (const auto& s : slots) {
                FVector fl, sl;
                FQuat fr;
                if (s.slot_id == row->attach_id && FittedPose(gun, s, *row, l.actor, &fl, &fr, &sl)) {
                    rot = fr;
                    break;
                }
            }
        }
        l.rest_rot = rot;
        l.have_rest = true;
        FVector spot = T.anchor + T.toward * 14.0 + T.along * ((i - (n - 1) * 0.5) * 9.0);
        double surface = T.surface_z;
        FVector at, normal;
        std::string what;
        if (TraceDown(L.player, spot + FVector{0, 0, 20}, spot - FVector{0, 0, 25}, ignore, &at, &normal, &what) &&
            normal.Z > 0.7)
            surface = at.Z;
        double min_z = 1e9;
        for (int k = 0; k < 8; ++k) {
            const FVector corner{k & 1 ? l.box.hi.X : l.box.lo.X, k & 2 ? l.box.hi.Y : l.box.lo.Y,
                                 k & 4 ? l.box.hi.Z : l.box.lo.Z};
            min_z = std::min(min_z, rot.Rotate(corner).Z);
        }
        FVector loc = spot - rot.Rotate(l.box.Center());
        loc.Z = surface + 0.3 - min_z;
        SetActorPose(l.actor, loc, rot);
        listed += (listed.empty() ? "" : ", ") + l.row;
    }
    log::Info("workbench: on the table beside the %s: %s", T.weapon.c_str(), listed.empty() ? "nothing" : listed.c_str());
}

void StartSession(UObject gun, const FVector& anchor, const FQuat& frame, double surface_z) {
    if (!L.have_db) {
        L.have_db = custom::Database(custom::GameInstance(L.player), &L.db);
        if (!L.have_db) log::Warn("workbench: could not read the game's parts database");
    }
    const bool fresh = !T.on || T.gun != gun;
    if (fresh) {
        for (auto& l : g_table) DestroyLoose(l);
        g_table.clear();
    }
    T.on = true;
    T.gun = gun;
    custom::WeaponRow(gun, &T.weapon_row);
    T.weapon = custom::NameString(T.weapon_row);
    T.anchor = anchor;
    FVector along = frame.Rotate(FVector{1, 0, 0});
    along.Z = 0;
    T.along = Norm(along);
    hands::Pose head;
    FVector to = hands::Head(&head) ? head.location - anchor : FVector{0, 0, 0} - T.along;
    to.Z = 0;
    to = to - T.along * Dot(to, T.along);
    T.toward = Len(to) > 1.0 ? Norm(to) : Norm(Cross(FVector{0, 0, 1}, T.along));
    T.surface_z = surface_z;
    custom::Purchased(custom::GameInstance(L.player), &T.owned);
    for (auto& l : g_table) l.have_rest = false;
    T.dirty = true;
}

// ---------------------------------------------------------------- grabbing
void BuildCandidate(int h, const FVector& hand, Candidate* best) {
    // Parts lying on the table.
    for (int i = 0; i < static_cast<int>(g_table.size()); ++i) {
        const Loose& l = g_table[i];
        if (!LiveObject(l.actor)) continue;
        const double d = DistToBox(hand, ActorLocation(l.actor), ActorRotation(l.actor), l.box);
        if (d < best->dist) *best = Candidate{1, i, l.actor, l.row, -1, d};
    }
    // Parts on the gun lying on the table.
    if (P.on && LiveObject(P.gun)) {
        const std::vector<custom::Slot> slots = PartSlots(P.gun);
        for (const auto& in : custom::InstalledParts(P.gun)) {
            if (in.path.size() != 1 || !SlotByIndex(slots, in.path[0]) || !LiveObject(in.actor)) continue;
            if (g_held[0].active && g_held[0].gun_part == in.actor) continue;
            if (g_held[1].active && g_held[1].gun_part == in.actor) continue;
            auto it = g_box_cache.find(in.actor);
            if (it == g_box_cache.end()) it = g_box_cache.emplace(in.actor, LocalBox(in.actor)).first;
            const double d = DistToBox(hand, ActorLocation(in.actor), ActorRotation(in.actor), it->second);
            if (d < best->dist) *best = Candidate{2, -1, in.actor, in.row, in.path[0], d};
        }
    }
    (void)h;
}

void Grab(int h, const Candidate& c, const hands::HandState& hs) {
    Held hd;
    hd.active = true;
    hd.row = c.row;
    const custom::PartRow* row = Part(c.row);
    if (row) hd.name = row->row;
    if (c.kind == 1) {
        if (c.index < 0 || c.index >= static_cast<int>(g_table.size()) || g_table[c.index].actor != c.actor) return;
        hd.actor = c.actor;
        hd.box = g_table[c.index].box;
        g_table.erase(g_table.begin() + c.index);
        log::Info("workbench: %s hand picked %s up from the table", HandName(h), c.row.c_str());
    } else {
        UObject cls = row ? custom::PartClass(*row) : ue::GetClass(c.actor);
        const FVector loc = ActorLocation(c.actor);
        const FQuat rot = ActorRotation(c.actor);
        UObject a = SpawnActor(L.player, cls ? cls : ue::GetClass(c.actor), loc, rot);
        if (!a) {
            log::Warn("workbench: could not take %s off the gun (no copy of it could be made)", c.row.c_str());
            return;
        }
        SetHidden(c.actor, true);
        hd.actor = a;
        hd.box = LocalBox(a);
        hd.from_gun = true;
        hd.gun_part = c.actor;
        hd.from_slot = c.slot;
        const custom::Slot* s = nullptr;
        const std::vector<custom::Slot> slots = PartSlots(P.gun);
        s = SlotByIndex(slots, c.slot);
        log::Info("workbench: %s hand took %s off the %s (slot %s)", HandName(h), c.row.c_str(), T.weapon.c_str(),
                  s ? s->name.c_str() : "?");
    }
    const FQuat inv = hs.grip.rotation.Inverse();
    hd.rel_rot = inv * ActorRotation(hd.actor);
    hd.rel_loc = inv.Rotate(ActorLocation(hd.actor) - hs.grip.location);
    g_held[h] = hd;
    input::Haptic(static_cast<input::Hand>(h), 0.35f, 0.03f);
}

// The gun the held part can go on: lying on the table, or in the right hand.
UObject TargetGun() {
    if (P.on && LiveObject(P.gun)) return P.gun;
    if (hands::GetGrab(hands::kRight).kind == hands::GrabKind::kGunGrip) {
        UObject g = CurrentGun();
        if (g && g == T.gun) return g;
    }
    return 0;
}

void FollowHand(int h, const hands::HandState& hs) {
    Held& hd = g_held[h];
    if (!LiveObject(hd.actor)) {
        hd = Held{};
        return;
    }
    FVector loc = hs.grip.location + hs.grip.rotation.Rotate(hd.rel_loc);
    FQuat rot = hs.grip.rotation * hd.rel_rot;
    // Near its place on a gun: it snaps there (shown fitted) until it is let
    // go or pulled away.
    int snapped = -1;
    std::string snapped_name;
    const custom::PartRow* row = Part(hd.row);
    UObject gun = TargetGun();
    if (row && gun) {
        UObject ap = 0;
        ue::GetProp(hd.actor, "AttachPoint", ap);
        const Xf rel = ap ? RelativeTransform(ap) : Xf{};
        const FVector point = loc + rot.Rotate(rel.translation);  // the part's attach point, where the hand has it
        double best = (hd.snapped >= 0 ? S.snap_cm + 2.0 : S.snap_cm);
        for (const auto& s : PartSlots(gun)) {
            if (s.slot_id != row->attach_id) continue;
            FVector fl, sl;
            FQuat fr;
            if (!FittedPose(gun, s, *row, hd.actor, &fl, &fr, &sl)) continue;
            const double d = Len(point - sl);
            if (d < best) {
                best = d;
                snapped = s.index;
                snapped_name = s.name;
                loc = fl;
                rot = fr;
            }
        }
    }
    if (snapped >= 0 && snapped != hd.snapped) {
        input::Haptic(static_cast<input::Hand>(h), 0.5f, 0.03f);
        log::Info("workbench: %s lines up with %s", hd.row.c_str(), snapped_name.c_str());
    }
    hd.snapped = snapped;
    hd.snapped_name = snapped_name;
    SetActorPose(hd.actor, loc, rot);
}

std::vector<custom::SavedPart> WithSlot(std::vector<custom::SavedPart> parts, int slot, const FName& row) {
    for (auto& p : parts)
        if (p.path.size() == 1 && p.path[0] == slot) {
            p.row = row;
            return parts;
        }
    parts.push_back(custom::SavedPart{row, {static_cast<uint8_t>(slot)}});
    return parts;
}

std::string ListParts(UObject gun) {
    std::string s;
    for (const auto& in : custom::InstalledParts(gun))
        s += (s.empty() ? "" : ", ") + in.row + custom::PathString(in.path);
    return s.empty() ? "none" : s;
}

// The game rebuilt the gun's parts: a part the other hand has taken off (and
// not settled yet) is a new actor now, shown again. Hide that one instead.
void RehideHeld(UObject gun) {
    for (auto& hd : g_held) {
        if (!hd.active || !hd.from_gun || LiveObject(hd.gun_part)) continue;
        hd.gun_part = 0;
        for (const auto& in : custom::InstalledParts(gun))
            if (in.path.size() == 1 && in.path[0] == hd.from_slot && in.row == hd.row) {
                hd.gun_part = in.actor;
                SetHidden(in.actor, true);
            }
    }
}

// Fits `row` into `slot` (or empties it with the row "Empty") through the
// game, then checks what the gun really carries.
bool Commit(UObject gun, int slot, const FName& row, const char* what) {
    UObject gi = custom::GameInstance(L.player);
    std::vector<custom::SavedPart> saved;
    if (!custom::Saved(gi, T.weapon_row, &saved) || saved.empty()) {
        log::Warn("workbench: no saved attachment list for %s, left as it is", T.weapon.c_str());
        return false;
    }
    const std::vector<custom::SavedPart> next = WithSlot(saved, slot, row);
    std::string why;
    if (!custom::ApplyLoadout(L.player, gun, next, &why)) {
        log::Warn("workbench: %s did not happen: %s", what, why.c_str());
        return false;
    }
    g_box_cache.clear();
    RehideHeld(gun);
    std::vector<custom::SavedPart> after;
    custom::Saved(gi, T.weapon_row, &after);
    std::string saved_s;
    for (const auto& p : after) saved_s += (saved_s.empty() ? "" : ", ") + custom::NameString(p.row) + custom::PathString(p.path);
    // Check: the slot holds the part now (or nothing, when emptied).
    const std::string want = custom::NameString(row);
    bool in_slot = false, slot_empty = true;
    for (const auto& in : custom::InstalledParts(gun))
        if (in.path.size() == 1 && in.path[0] == slot) {
            slot_empty = false;
            if (in.row == want) in_slot = true;
        }
    const bool ok = want == "Empty" ? slot_empty : in_slot;
    log::Info("workbench: %s: %s. The gun now carries %s; saved: %s", what, ok ? "done, checked on the gun" :
              "the game rebuilt the gun but the slot does not show it", ListParts(gun).c_str(), saved_s.c_str());
    return ok;
}

void Release(int h) {
    Held hd = g_held[h];
    g_held[h] = Held{};
    if (!hd.active) return;
    UObject gun = TargetGun();
    const custom::PartRow* row = Part(hd.row);
    const std::vector<custom::Slot> slots = gun ? PartSlots(gun) : std::vector<custom::Slot>{};
    if (hd.snapped >= 0 && gun && row) {
        if (hd.from_gun && hd.snapped == hd.from_slot) {
            if (LiveObject(hd.gun_part)) SetHidden(hd.gun_part, false);
            DestroyActor(hd.actor);
            log::Info("workbench: %s put back on (nothing changed)", hd.row.c_str());
            return;
        }
        // Fitted: the game makes its own part actor; ours goes.
        char what[160];
        snprintf(what, sizeof(what), "fit %s in %s", hd.row.c_str(), hd.snapped_name.c_str());
        if (Commit(gun, hd.snapped, hd.name, what)) {
            DestroyActor(hd.actor);
            input::Haptic(static_cast<input::Hand>(h), 0.8f, 0.05f);
            T.dirty = true;  // a part it replaced lands on the table
            return;
        }
        g_held[h] = hd;
        ReturnHeld(h, "(the game did not take it)");
        return;
    }
    if (hd.from_gun) {
        const custom::Slot* s = SlotByIndex(slots, hd.from_slot);
        if (!gun || !s || !s->can_be_empty) {
            g_held[h] = hd;
            ReturnHeld(h, s && !s->can_be_empty ? "on the gun: the game always needs a part there" : "on the gun");
            input::Haptic(static_cast<input::Hand>(h), 0.2f, 0.08f);
            return;
        }
        FName empty;
        if (!ue::names::Find("Empty", &empty)) {
            g_held[h] = hd;
            ReturnHeld(h, "on the gun (no \"Empty\" name in the game)");
            return;
        }
        char what[160];
        snprintf(what, sizeof(what), "take %s off %s", hd.row.c_str(), s->name.c_str());
        if (Commit(gun, hd.from_slot, empty, what)) {
            // The game destroyed its part; ours goes on the table.
            Loose l;
            l.row = hd.row;
            l.name = hd.name;
            l.actor = hd.actor;
            l.box = hd.box;
            g_table.push_back(l);
            T.dirty = true;
            return;
        }
        g_held[h] = hd;
        ReturnHeld(h, "on the gun (the game did not take it off)");
        return;
    }
    g_held[h] = hd;
    ReturnHeld(h, "on the table");
}

// ---------------------------------------------------------------- per frame
void SearchLevel(UObject player) {
    if (L.searched || L.age <= 4.0 || g_search_state.load() != 0) return;
    L.searched = true;
    g_search_for = player;
    g_search_state = 1;
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            const UObject bench_cls = g_bench_class.Get(), facility_cls = g_facility_class.Get();
            g_found_bench = bench_cls ? ue::FindFirstInstance(bench_cls) : 0;
            g_found_facility = facility_cls ? ue::FindFirstInstance(facility_cls) : 0;
            g_search_state = 2;
            return 0;
        }, nullptr, 0, nullptr))
        CloseHandle(t);
    else
        g_search_state = 0;
}

void CheckFacility() {
    if (L.age < L.next_state_check) return;
    L.next_state_check = L.age + 2.0;
    if (!LiveObject(L.bench)) return;
    L.bench_loc = ActorLocation(L.bench);
    uint8_t st = 0xFF;
    const bool have = L.facility && LiveObject(L.facility) && ue::GetProp(L.facility, "FacilityState", st);
    const int state = have ? st : -1;
    // A locked bench is parked at the world origin.
    const bool unlocked = state == kFacilityUnlocked && Len(L.bench_loc) > 1.0;
    if (state != L.facility_state || unlocked != L.unlocked) {
        L.facility_state = state;
        L.unlocked = unlocked;
        log::Info("workbench: workbench %s (facility state %d, bench at %.0f %.0f %.0f)%s",
                  unlocked ? "UNLOCKED" : "locked", state, L.bench_loc.X, L.bench_loc.Y, L.bench_loc.Z,
                  unlocked ? ": the pistol can be laid on it"
                           : (S.tables_before_unlock ? ": any table in the hub works for now" : ""));
    }
}

void OnTick(UObject player, float dt) {
    if (player != L.player) {
        EndSession("new level", true);
        L = Level();
        L.player = player;
        g_claim[0] = g_claim[1] = Claim{};
    }
    if (!player) return;
    L.age += dt;
    SearchLevel(player);
    if (g_search_state.load() == 2) {
        g_search_state = 0;
        if (g_search_for.load() == player) {
            L.bench = g_found_bench.load();
            L.facility = g_found_facility.load();
        }
        if (L.bench) {
            const FVector b = ActorLocation(L.bench);
            log::Info("workbench: %s in this level at (%.0f %.0f %.0f), %.0f cm from the player; facility %s",
                      ue::GetName(L.bench).c_str(), b.X, b.Y, b.Z, Len(b - ActorLocation(player)),
                      L.facility ? ue::GetName(L.facility).c_str() : "not found");
        }
    }
    if (!L.bench) return;
    if (S.study && !L.studied && L.age > 6.0) {
        L.studied = true;
        custom::Study(player, L.bench);
    }
    CheckFacility();
    if (!S.enabled) return;

    // The parked gun went away (switched weapon, the game replaced it).
    if (P.on && (!LiveObject(P.gun) || CurrentGun() != P.gun)) EndSession("the pistol is gone from the table");
    if (T.on && (!LiveObject(T.gun) || CurrentGun() != T.gun)) EndSession("weapon changed");
    // Walked away from the table.
    if (T.on && Flat(ActorLocation(player), T.anchor) > S.walk_away_cm) EndSession("walked away");

    const input::Snapshot snap = input::LastConsumed();
    for (int h = 0; h < 2; ++h) {
        const hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
        const input::HandInput& in = snap.hand[h];
        if (g_held[h].active) {
            if (!hs.valid) continue;
            if (!in.grip_button.down) {
                Release(h);
            } else {
                FollowHand(h, hs);
            }
            continue;
        }
        const bool fresh = g_claim[h].time == hs.time && g_claim[h].c.kind != 0;
        if (fresh && !g_hover[h]) input::Haptic(static_cast<input::Hand>(h), 0.12f, 0.012f);
        g_hover[h] = fresh;
        if (fresh && in.grip_button.pressed && hs.valid) Grab(h, g_claim[h].c, hs);
    }
    if (T.on && T.dirty) LayOutTable();

    // First time a hand comes near the bench in a level (geometry check).
    if (L.age >= L.next_near_check && !L.logged_near && L.unlocked) {
        L.next_near_check = L.age + 1.0;
        for (int h = 0; h < 2; ++h) {
            hands::HandState hs = hands::Get(static_cast<hands::Hand>(h));
            if (!hs.valid) continue;
            const double d = Len(hs.grip.location - L.bench_loc);
            if (d < 120.0) {
                L.logged_near = true;
                log::Info("workbench: %s hand at the bench: (%.0f %.0f %.0f), %.0f cm from the bench origin "
                          "(%.0f cm above it)",
                          HandName(h), hs.grip.location.X, hs.grip.location.Y, hs.grip.location.Z, d,
                          hs.grip.location.Z - L.bench_loc.Z);
                break;
            }
        }
    }
}

}  // namespace

bool ParkedGun(UObject gun, FVector* spot, FQuat* frame) {
    if (!P.on || P.gun != gun || !gun) return false;
    *spot = P.spot;
    *frame = P.frame;
    return true;
}

bool TryPark(UObject gun, const FQuat& frame, const FVector& hand) {
    P.on = false;
    if (!S.enabled || !L.player || !L.bench || !gun) return false;
    if (L.unlocked) {
        if (Flat(hand, L.bench_loc) > S.bench_reach_cm) return false;
    } else if (!S.tables_before_unlock) {
        return false;
    }
    std::vector<UObject> ignore{L.player, gun};
    AddAttached(gun, &ignore);
    AddAttached(L.player, &ignore);
    for (auto& l : g_table) ignore.push_back(l.actor);
    for (auto& hd : g_held)
        if (hd.active) ignore.push_back(hd.actor);
    FVector at, normal;
    std::string what;
    if (!TraceDown(L.player, hand + FVector{0, 0, 5}, hand - FVector{0, 0, 35}, ignore, &at, &normal, &what)) return false;
    if (normal.Z < 0.8) {
        log::Info("workbench: let go over %s, but it is not flat enough to lay the pistol on", what.c_str());
        return false;
    }
    // Lying on its side, barrel where it pointed, the side that faced up
    // still up.
    FVector x = frame.Rotate(FVector{1, 0, 0});
    x.Z = 0;
    if (Len(x) < 0.2) {
        hands::Pose head;
        x = hands::Head(&head) ? head.rotation.Rotate(FVector{1, 0, 0}) : FVector{1, 0, 0};
        x.Z = 0;
    }
    x = Norm(x);
    const bool right_up = frame.Rotate(FVector{0, 1, 0}).Z >= 0;
    const FVector y = right_up ? FVector{0, 0, 1} : FVector{0, 0, -1};
    const FVector z = Cross(x, y);
    P.on = true;
    P.gun = gun;
    P.frame = FromAxes(x, y, z);
    P.spot = FVector{hand.X, hand.Y, at.Z + S.rest_cm};
    P.surface_z = at.Z;
    StartSession(gun, P.spot, P.frame, at.Z);
    log::Info("workbench: pistol laid on %s at (%.0f %.0f %.0f), %s side up; it carries %s", what.c_str(), at.X,
              at.Y, at.Z, right_up ? "right" : "left", ListParts(gun).c_str());
    return true;
}

void GunTaken() {
    if (!P.on) return;
    P.on = false;
    log::Info("workbench: pistol picked up from the table");
}

bool ClaimsGrip(int hand) {
    if (hand < 0 || hand > 1 || !S.enabled) return false;
    if (g_held[hand].active) return true;
    if (!T.on) return false;
    const hands::HandState hs = hands::Get(static_cast<hands::Hand>(hand));
    Claim& c = g_claim[hand];
    if (c.time == hs.time) return c.c.kind != 0;
    c.time = hs.time;
    c.c = Candidate{};
    if (!hs.valid) return false;
    // A hand busy with something of controls' (slide, magazine, the gun) is
    // left alone.
    const hands::GrabKind busy = hands::GetGrab(static_cast<hands::Hand>(hand)).kind;
    if (busy != hands::GrabKind::kNone && busy != hands::GrabKind::kHolster) return false;
    Candidate best;
    best.dist = S.grab_cm;
    BuildCandidate(hand, hs.grip.location, &best);
    if (best.kind == 0) return false;
    // The right hand at the gun lying on the table: the gun wins unless the
    // hand is closer to the part than to where the gun is held.
    if (hand == 1 && P.on && Len(hs.grip.location - P.spot) <= best.dist) return false;
    c.c = best;
    return true;
}

void Init() {
    S.study = config::GetBool("workbench_study", true);
    S.enabled = config::GetBool("workbench_hands", true);
    S.tables_before_unlock = config::GetBool("workbench_tables_before_unlock", true);
    S.grab_cm = config::GetFloat("workbench_grab_cm", 3.0f);
    S.snap_cm = config::GetFloat("workbench_snap_cm", 5.0f);
    S.walk_away_cm = config::GetFloat("workbench_walk_away_m", 3.0f) * 100.0;
    game::AddTickListener(&OnTick);
}

}  // namespace holevr::vr::workbench
