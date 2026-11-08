#include "vr/gun_melee.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "ue/names.h"
#include "ue/objects.h"
#include "ue/reflection.h"
#include "vr/gun_melee_math.h"
#include "vr/input.h"
#include "vr/stereo.h"

namespace holevr::vr::gun_melee {
namespace {
using ue::UObject;

// The game's own trace channel for bullets and the kick (TraceTypeQuery5:
// BP_SingleBullet_LineTrace and BP_Player.KickTrace both pass 4).
constexpr uint8_t kGameHitChannel = 4;

struct Settings {
    bool on = true;
    bool stance_on = true;
    StanceTuning stance;
    Tuning pistol, long_gun;
    double pistol_push = 6000.0, long_push = 10000.0;  // shove on a body that is already a ragdoll
} S;

double Setting(const char* key, double fallback, double lo, double hi) {
    const double v = config::GetFloat(key, static_cast<float>(fallback));
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

void LoadSettings() {
    S.on = config::GetBool("gun_melee", true);
    Tuning t;
    t.min_speed = Setting("gun_melee_min_speed", 2.0, 0.5, 10.0);
    t.full_speed = std::max(t.min_speed + 0.1, Setting("gun_melee_full_speed", 4.5, 0.6, 15.0));
    t.cooldown_s = Setting("gun_melee_cooldown_s", 0.45, 0.1, 3.0);
    S.pistol = S.long_gun = t;
    S.pistol.damage_share = Setting("gun_melee_pistol_damage", 0.15, 0.0, 2.0);
    S.pistol.stun_share = Setting("gun_melee_pistol_stun", 0.7, 0.0, 5.0);
    S.long_gun.damage_share = Setting("gun_melee_long_gun_damage", 0.25, 0.0, 2.0);
    S.long_gun.stun_share = Setting("gun_melee_long_gun_stun", 1.2, 0.0, 5.0);
    S.stance_on = config::GetBool("butt_strike_pose", true);
    S.stance.near_head_cm = Setting("butt_strike_pose_near_head_cm", 30.0, 5.0, 80.0);
    S.stance.off_above_cm = Setting("butt_strike_pose_off_hand_above_cm", 10.0, -40.0, 80.0);
    S.stance.off_view_deg = Setting("butt_strike_pose_barrel_off_view_deg", 50.0, 0.0, 180.0);
    S.stance.hold_s = Setting("butt_strike_pose_hold_s", 0.5, 0.0, 3.0);
}

perf::Counter g_perf{"gun melee"};

// ---- engine objects, looked up once ----
struct Libs {
    UObject ksl = 0, gps = 0, niagara = 0, character = 0;
    int tries = 0;
    bool Ready() {
        if (ksl && gps && character) return true;
        if (tries >= 3) return false;
        ++tries;
        if (!ksl) ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!gps) gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        if (!niagara) niagara = ue::FindObject("NiagaraFunctionLibrary /Script/Niagara.Default__NiagaraFunctionLibrary");
        if (!character) character = ue::FindObject("Class /Script/Engine.Character");
        if (!(ksl && gps && character))
            log::Warn("gun melee: engine libraries not found (trace %s, statics %s, character %s); off",
                      ksl ? "ok" : "missing", gps ? "ok" : "missing", character ? "ok" : "missing");
        return ksl && gps && character;
    }
} L;

// Sound and splash the game's own melee weapons use on a blunt hit. Looked
// up at the first hit only (a full object search), then kept.
ue::ObjectRef g_hit_sound{"MetaSoundSource /Game/Weapons/Sounds/Melee/S_MeleeHit_Hammer_Fresh.S_MeleeHit_Hammer_Fresh",
                          120.0};
ue::ObjectRef g_kick_sound{"MetaSoundSource /Game/Weapons/Sounds/Melee/S_MeleeHit_Kick.S_MeleeHit_Kick", 120.0};
ue::ObjectRef g_hit_splash{
    "NiagaraSystem /Game/Weapons/Custom/Parts/GunParts/01_Ammo/FX/FX_HitSplash_Simple.FX_HitSplash_Simple", 120.0};

bool Live(UObject o) { return o && ue::IsLive(o); }

FRotator DirToRotator(const FVector& d) {
    return FRotator{std::atan2(d.Z, std::sqrt(d.X * d.X + d.Y * d.Y)) * kRadToDeg, std::atan2(d.Y, d.X) * kRadToDeg, 0};
}

bool HasParam(const ue::Call& c, const char* name, size_t size) {
    int32_t got = 0;
    return c.Raw(name, &got) && got == static_cast<int32_t>(size);
}

// ---- who can be hit ----
// Anything with the game's bullet-hit event (enemies, the nurse, the hub's
// mannequins and shooting targets, breakable boards), except the player.
std::unordered_map<UObject, bool> g_hittable_class;
std::unordered_map<UObject, bool> g_npc_class;
UObject g_classes_player = 0;   // the class tables belong to this player's level

bool ClassHas(std::unordered_map<UObject, bool>& table, UObject cls, bool (*test)(UObject)) {
    if (!cls) return false;
    auto it = table.find(cls);
    if (it != table.end()) return it->second;
    const bool yes = test(cls);
    table.emplace(cls, yes);
    return yes;
}
bool HasHitEvent(UObject cls) { return ue::FindFunction(cls, "BulletHitEvent") != 0; }
bool IsNpcBase(UObject cls) {
    for (UObject s = cls; s; s = ue::reflection::GetSuper(s))
        if (ue::GetName(s) == "BP_NPCBase_C") return true;
    return false;
}

// Enemies (and other game characters) near enough to be hit, refreshed
// twice a second. One kept call: the engine reuses its result array.
struct Npc { UObject actor = 0; };
std::vector<Npc> g_npcs;
double g_npcs_at = -1e9;
void RefreshNpcs(UObject player, double clock) {
    if (clock - g_npcs_at < 0.5 && clock >= g_npcs_at) return;
    g_npcs_at = clock;
    g_npcs.clear();
    static ue::Call* all = nullptr;
    static bool usable = false;
    if (!all) {
        all = new ue::Call(L.gps, "GetAllActorsOfClass");
        usable = all->Valid() && HasParam(*all, "OutActors", 16) && HasParam(*all, "ActorClass", sizeof(UObject));
        if (!usable) log::Warn("gun melee: GetAllActorsOfClass unusable; only the world check finds targets");
    }
    if (!usable) return;
    all->Set("WorldContextObject", player).Set("ActorClass", L.character);
    if (!all->Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = all->Raw("OutActors", &size);
    struct Arr { uintptr_t data; int32_t num, max; } arr{};
    if (!raw || size < static_cast<int32_t>(sizeof(Arr))) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    if (arr.num < 0 || arr.num > arr.max || arr.num > 4096) return;
    for (int32_t i = 0; i < arr.num && g_npcs.size() < 48; ++i) {
        const UObject a = mem::ReadOr<uintptr_t>(arr.data + static_cast<uintptr_t>(i) * 8u, 0);
        if (!a || a == player || !Live(a)) continue;
        if (ClassHas(g_npc_class, ue::GetClass(a), &IsNpcBase)) g_npcs.push_back({a});
    }
}

FVector ActorLoc(UObject a) {
    ue::Call c(a, "K2_GetActorLocation");
    return c.Invoke() ? c.Result<FVector>() : FVector{1e9, 1e9, 1e9};
}

// ---- one contact ----
struct Contact {
    UObject actor = 0, comp = 0;
    FVector point, normal;
    ue::FName bone{};
    uint8_t surface = 0;
    bool npc = false;
    bool blocking = false;   // the sweep stopped here (else the ball passed through it)
    bool initial = false;    // the ball was already in it where the sweep started
    double dist = 0;         // cm along the sweep
    const char* found_by = "";
};

// Copies a hit result into BreakHitResult/GetSurfaceType and reads it.
struct HitReader {
    ue::Call unpack{L.gps, "BreakHitResult"};
    ue::Call surface{L.gps, "GetSurfaceType"};
    int32_t size = 0, distance_size = 0;
    bool ok = false, has_initial = false;
    HitReader() {
        int32_t s2 = 0;
        has_initial = HasParam(unpack, "bInitialOverlap", sizeof(bool));
        unpack.Raw("Distance", &distance_size);
        ok = unpack.Valid() && unpack.Raw("Hit", &size) && size > 0 && surface.Valid() && surface.Raw("Hit", &s2) &&
             s2 == size && HasParam(unpack, "HitActor", sizeof(UObject)) &&
             HasParam(unpack, "HitComponent", sizeof(UObject)) && HasParam(unpack, "ImpactPoint", sizeof(FVector)) &&
             HasParam(unpack, "ImpactNormal", sizeof(FVector)) && HasParam(unpack, "bBlockingHit", sizeof(bool)) &&
             HasParam(unpack, "HitBoneName", sizeof(ue::FName)) && HasParam(surface, "ReturnValue", 1);
        if (!ok) log::Warn("gun melee: hit results unreadable (BreakHitResult / GetSurfaceType); off");
    }
    bool Read(const uint8_t* hit, int32_t hit_size, Contact* c) {
        if (!ok || !hit || hit_size != size) return false;
        int32_t n = 0;
        uint8_t* in = unpack.Raw("Hit", &n);
        uint8_t* in2 = surface.Raw("Hit", &n);
        std::memcpy(in, hit, static_cast<size_t>(size));
        std::memcpy(in2, hit, static_cast<size_t>(size));
        if (!unpack.Invoke()) return false;
        c->actor = unpack.Get<UObject>("HitActor");
        c->comp = unpack.Get<UObject>("HitComponent");
        c->point = unpack.Get<FVector>("ImpactPoint");
        c->normal = unpack.Get<FVector>("ImpactNormal");
        c->bone = unpack.Get<ue::FName>("HitBoneName");
        c->blocking = unpack.Get<bool>("bBlockingHit");
        c->initial = has_initial && unpack.Get<bool>("bInitialOverlap");
        c->dist = -1;
        if (distance_size == 4) c->dist = unpack.Get<float>("Distance", -1.0f);
        else if (distance_size == 8) c->dist = unpack.Get<double>("Distance", -1.0);
        surface.Set("ReturnValue", uint8_t{0});
        c->surface = surface.Invoke() ? surface.Result<uint8_t>() : 0;
        return true;
    }
};
HitReader* Reader() {
    static HitReader* r = new HitReader();
    return r->ok ? r : nullptr;
}

// A ball swept from where the strike point was to where it is, the way the
// game's kick looks for what it hits (SphereTraceMulti): everything the ball
// passes through, nearest first, then the one thing that stops it. The hub's
// mannequins and targets only overlap the game's hit channel (bullets and the
// kick go through them); a single sweep reports only what stops it, so it
// never saw them (owner 2026-11-08: no damage, no stun, no sound on the hub
// dummies).
constexpr int kMaxTouches = 12;
struct MultiSweep {
    ue::Call t{L.ksl, "SphereTraceMulti"};
    int32_t radius_size = 0;
    bool ok = false;
    std::vector<uint8_t> one;   // one hit result copied out of the engine's array
    MultiSweep() {
        ok = t.Valid() && t.Raw("Radius", &radius_size) && (radius_size == 4 || radius_size == 8) &&
             HasParam(t, "OutHits", 16) && HasParam(t, "Start", sizeof(FVector)) &&
             HasParam(t, "End", sizeof(FVector)) && HasParam(t, "TraceChannel", 1) &&
             HasParam(t, "bTraceComplex", sizeof(bool)) && HasParam(t, "ActorsToIgnore", 16) &&
             HasParam(t, "bIgnoreSelf", sizeof(bool));
        if (!ok) log::Warn("gun melee: SphereTraceMulti unusable; only enemies' own shapes are checked");
    }
    // What the ball met, nearest first (at most `max`). The engine keeps its
    // own result array in this call and reuses its memory.
    int Run(UObject player, const FVector& a, FVector b, double r, uint8_t channel, bool complex,
            const std::vector<UObject>& ignore, Contact* out, int max) {
        HitReader* rd = Reader();
        if (!ok || !rd) return 0;
        if (Length(b - a) < 1e-3) b = a + FVector{0.01, 0, 0};
        struct Arr { uintptr_t data; int32_t num, max; };
        int32_t raw_size = 0;
        uint8_t* raw = t.Raw("OutHits", &raw_size);
        if (!raw || raw_size < static_cast<int32_t>(sizeof(Arr))) return 0;
        Arr hits{};
        std::memcpy(&hits, raw, sizeof(Arr));
        hits.num = 0;   // start empty, keep the engine's memory
        std::memcpy(raw, &hits, sizeof(Arr));
        struct { const UObject* data; int32_t num, max; } arr{ignore.data(), static_cast<int32_t>(ignore.size()),
                                                             static_cast<int32_t>(ignore.size())};
        t.Set("WorldContextObject", player).Set("Start", a).Set("End", b).Set("TraceChannel", channel)
            .Set("bTraceComplex", complex).Set("ActorsToIgnore", arr).Set("DrawDebugType", uint8_t{0})
            .Set("bIgnoreSelf", true).Set("ReturnValue", false);
        if (radius_size == 4) t.Set("Radius", static_cast<float>(r));
        else t.Set("Radius", r);
        // The return value says only whether something stopped the ball:
        // what it passed through is in the array either way.
        if (!t.Invoke()) return 0;
        std::memcpy(&hits, raw, sizeof(Arr));
        if (!hits.data || hits.num <= 0 || hits.num > hits.max || hits.num > 256) return 0;
        one.resize(static_cast<size_t>(rd->size));
        int got = 0;
        for (int32_t i = 0; i < hits.num && got < max; ++i) {
            const uintptr_t p = hits.data + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(rd->size);
            if (!mem::IsReadable(p, one.size()) ||
                !mem::SafeCopy(one.data(), reinterpret_cast<const void*>(p), one.size()))
                break;
            Contact c;
            if (!rd->Read(one.data(), rd->size, &c)) continue;
            if (!(c.dist >= 0) || !std::isfinite(c.dist)) c.dist = Length(c.point - a);
            // Already inside it at the start: no impact point to speak of.
            if (c.initial || !std::isfinite(c.point.X) || Length(c.point) < 1e-3) c.point = b, c.dist = 0;
            out[got++] = c;
        }
        return got;
    }
};
MultiSweep* Sweeper() {
    static MultiSweep* w = new MultiSweep();
    return w;
}

// An enemy's own body shapes (its physics asset), swept the same way. Works
// whatever the enemy's collision answers to the game's channels.
bool BodySweep(UObject mesh, const FVector& a, FVector b, double r, Contact* c) {
    if (!Live(mesh)) return false;
    ue::Call t(mesh, "K2_SphereTraceComponent");
    int32_t radius_size = 0, hit_size = 0;
    if (!t.Valid() || !t.Raw("SphereRadius", &radius_size) || !t.Raw("OutHit", &hit_size)) return false;
    if (Length(b - a) < 1e-3) b = a + FVector{0.01, 0, 0};
    t.Set("TraceStart", a).Set("TraceEnd", b).Set("bTraceComplex", false).Set("bShowTrace", false)
        .Set("bPersistentShowTrace", false).Set("ReturnValue", false);
    if (radius_size == 4) t.Set("SphereRadius", static_cast<float>(r));
    else if (radius_size == 8) t.Set("SphereRadius", r);
    else return false;
    if (!t.Invoke() || !t.Result<bool>()) return false;
    int32_t n = 0;
    const uint8_t* hit = t.Raw("OutHit", &n);
    HitReader* rd = Reader();
    if (!rd || !rd->Read(hit, n, c)) return false;
    c->comp = mesh;
    const ue::FName bone = t.Get<ue::FName>("BoneName");
    if (bone.comparison_index) c->bone = bone;
    const FVector at = t.Get<FVector>("HitLocation");
    if (std::isfinite(at.X) && Length(at) > 0) c->point = at;
    return true;
}

// A component sweep brings no physical material, so the body part's surface
// comes from the enemy's physics asset: each bone's body names its material.
std::unordered_map<UObject, std::vector<std::pair<uint32_t, uint8_t>>> g_bone_surfaces;  // per physics asset
bool SurfaceOfBone(UObject mesh, const ue::FName& bone, uint8_t* out) {
    if (!bone.comparison_index) return false;
    ue::Call pa(mesh, "GetPhysicsAsset");
    const UObject asset = pa.Invoke() ? pa.Result<UObject>() : 0;
    if (!Live(asset)) return false;
    auto it = g_bone_surfaces.find(asset);
    if (it == g_bone_surfaces.end()) {
        std::vector<std::pair<uint32_t, uint8_t>> list;
        struct Arr { uintptr_t data; int32_t num, max; } arr{};
        const int32_t o = ue::PropertyOffset(asset, "SkeletalBodySetups");
        if (o >= 0 && mem::Read(asset + o, arr) && arr.num > 0 && arr.num <= arr.max && arr.num < 512) {
            for (int32_t i = 0; i < arr.num; ++i) {
                const UObject setup = mem::ReadOr<uintptr_t>(arr.data + static_cast<uintptr_t>(i) * 8u, 0);
                ue::FName name{};
                UObject mat = 0;
                uint8_t surface = 0;
                if (!setup || !ue::GetProp(setup, "BoneName", name)) continue;
                if (ue::GetProp(setup, "PhysMaterial", mat) && mat) ue::GetProp(mat, "SurfaceType", surface);
                list.emplace_back(name.comparison_index, surface);
            }
        }
        log::Info("gun melee: %zu body parts with their materials read from %s", list.size(), ue::GetName(asset).c_str());
        it = g_bone_surfaces.emplace(asset, std::move(list)).first;
    }
    for (const auto& [name, surface] : it->second)
        if (name == bone.comparison_index) { *out = surface; return true; }
    return false;
}

// ---- the game's own numbers for a hit ----
UObject GameInstance(UObject player) {
    ue::Call c(L.gps, "GetGameInstance");
    c.Set("WorldContextObject", player);
    return c.Invoke() ? c.Result<UObject>() : 0;
}
double DamageMultiplier(UObject gi, uint8_t surface, bool penetrated) {
    ue::Call c(gi, "GetHitDamageMultiplierData");
    if (!c.Valid()) return 1.0;
    c.Set("HitSurfaceType", surface).Set("IsArmorPenetrated", penetrated).Set("DamageMultiplier", 1.0);
    return c.Invoke() ? c.Get<double>("DamageMultiplier", 1.0) : 1.0;
}
double StunMultiplier(UObject gi, uint8_t surface) {
    ue::Call c(gi, "GetHitStunDamageMultiplierData");
    if (!c.Valid()) return 1.0;
    c.Set("HitSurface", surface).Set("Multiplier", 1.0);
    return c.Invoke() ? c.Get<double>("Multiplier", 1.0) : 1.0;
}
int ArmorPoint(UObject gi, uint8_t surface) {
    ue::Call c(gi, "GetArmorPointData");
    if (!c.Valid()) return 0;
    c.Set("SurfaceType", surface).Set("ArmorPoint", int32_t{0});
    return c.Invoke() ? c.Get<int32_t>("ArmorPoint", 0) : 0;
}
double NumberProp(UObject obj, const char* name, double fallback);

// The target's HP: enemies answer GetHP; the hub's mannequins have no such
// function, only their HP and MaxHP numbers.
bool GetHp(UObject target, double* hp, double* max_hp) {
    ue::Call c(target, "GetHP");
    if (c.Valid() && HasParam(c, "HP", sizeof(double)) && c.Invoke()) {
        *hp = c.Get<double>("HP", -1.0);
        *max_hp = c.Get<double>("MaxHP", -1.0);
        return std::isfinite(*hp) && std::isfinite(*max_hp);
    }
    const double nan = std::nan("");
    *hp = NumberProp(target, "HP", nan);
    *max_hp = NumberProp(target, "MaxHP", nan);
    return std::isfinite(*hp) && std::isfinite(*max_hp) && *max_hp > 0;
}

// A number property read by its real type (Blueprint numbers are doubles in
// UE5, but a C++ one can be a float).
double NumberProp(UObject obj, const char* name, double fallback) {
    if (!obj) return fallback;
    for (UObject cls = ue::GetClass(obj); cls; cls = ue::reflection::GetSuper(cls)) {
        for (const auto& p : ue::reflection::GetProperties(cls)) {
            if (p.name != name) continue;
            if (p.type == "DoubleProperty" && p.element_size == 8) {
                double v = fallback;
                return mem::Read(obj + p.offset, v) && std::isfinite(v) ? v : fallback;
            }
            if (p.type == "FloatProperty" && p.element_size == 4) {
                float v = 0;
                return mem::Read(obj + p.offset, v) && std::isfinite(v) ? v : fallback;
            }
            return fallback;
        }
    }
    return fallback;
}

bool g_told_feedback = false;
void Feedback(UObject player, const Contact& c, const FVector& dir, double swing) {
    UObject sound = g_hit_sound.Get();
    if (!sound) sound = g_kick_sound.Get();
    if (!g_told_feedback) {
        g_told_feedback = true;
        log::Info("gun melee: hit sound %s, hit splash %s", sound ? ue::GetName(sound).c_str() : "not loaded",
                  g_hit_splash.Get() ? "ready" : "not loaded");
    }
    if (L.gps) {
        if (sound) {
            ue::Call p(L.gps, "PlaySoundAtLocation");
            p.Set("WorldContextObject", player).Set("Sound", sound).Set("Location", c.point)
                .Set("VolumeMultiplier", static_cast<float>(0.6 + 0.4 * swing)).Set("PitchMultiplier", 1.0f);
            p.Invoke();
        }
    }
    if (L.niagara) {
        if (const UObject fx = g_hit_splash.Get()) {
            ue::Call s(L.niagara, "SpawnSystemAtLocation");
            s.Set("WorldContextObject", player).Set("SystemTemplate", fx).Set("Location", c.point)
                .Set("Rotation", DirToRotator(dir)).Set("Scale", FVector{1, 1, 1})
                .Set("bAutoDestroy", true).Set("bAutoActivate", true).Set("PoolingMethod", uint8_t{0})
                .Set("bPreCullCheck", true);
            s.Invoke();
        }
    }
}

// ---- per held gun ----
struct Track {
    UObject gun = 0;
    int hand = -1;
    bool pistol = true;
    int n = 0;
    StrikePoint pts[kMaxPoints];
    FVector prev_w[kMaxPoints], prev_t[kMaxPoints];
    int64_t xr_time = 0;
    uint64_t frame = 0;
    bool have = false;
    double since = 0;   // clock when this gun came into this hand
    // The swing going on (fast frames in a row), for the log.
    struct Swing {
        int frames = 0;
        double top = 0, t0 = 0;
        bool hit = false, pistol = true;
        const char* part = "";
        double nearest_enemy = 1e9;     // cm, from the strike point
        std::vector<std::string> met;   // what the sweeps met, each once
    } swing;
};
Track g_track[2];
Cooldowns g_cool;
UObject g_logged_points_for[2] = {0, 0};

// Session counts for the log.
struct Counts {
    int fast = 0, swings = 0, hits = 0, jumps = 0;
    int by[4] = {0, 0, 0, 0};   // hits found by: hit channel, visibility, shapes you see, an enemy's body
    double next_log = 0;
    bool dirty = false;
} g_counts;
constexpr int kMaxMissLines = 40;
int g_miss_lines = 0;

const char* TargetKind(const Contact& c) { return c.npc ? "enemy" : "target"; }

void Apply(UObject player, const HeldGun& g, const StrikePoint& pt, double speed, const FVector& dir, Contact c,
           double clock) {
    const Tuning& t = g.pistol ? S.pistol : S.long_gun;
    const double swing = SwingScale(speed, t.min_speed, t.full_speed);
    if (swing <= 0) return;
    if (!c.surface && c.npc && c.comp) SurfaceOfBone(c.comp, c.bone, &c.surface);
    const UObject gi = GameInstance(player);
    const int armor = gi ? ArmorPoint(gi, c.surface) : 0;
    const bool penetrated = armor <= 0;   // a blunt hit has no armour piercing: armour soaks it, like the game's melee
    const double dmul = gi ? DamageMultiplier(gi, c.surface, penetrated) : 1.0;
    const double smul = gi ? StunMultiplier(gi, c.surface) : 1.0;
    const double kick_stun = NumberProp(player, "KickStunPower", 0.0);
    double hp = -1, max_hp = -1;
    const bool has_hp = GetHp(c.actor, &hp, &max_hp);
    // A dead enemy is left alone; a mannequin at 0 HP still takes the hit
    // (the game's own kick never asks).
    const bool dead = c.npc && has_hp && hp <= 0;
    const double damage = Damage(t, has_hp ? max_hp : -1.0, swing, dmul);
    const double stun = Stun(t, kick_stun, swing, smul);
    const std::string bone = c.bone.comparison_index ? ue::names::ToString(c.bone) : std::string("-");
    if (!dead) {
        ue::Call e(c.actor, "BulletHitEvent");
        if (e.Valid()) {
            e.Set("HitLocation", c.point).Set("HitDirection", dir).Set("HitDamage", damage)
                .Set("HitStunPower", stun).Set("DoT_Damage", 0.0).Set("DoT_Stun", 0.0).Set("DoT_Time", 0.0)
                .Set("DoT_Interval", 0.0).Set("IsPenetrated", penetrated).Set("HitBoneName", c.bone)
                .Set("AttackLocation", ActorLoc(player)).Set("ShooterActor", player)
                .Set("HitSurfaceType", c.surface).Set("IsMultipleShot", false)
                .Set("ShotID", ue::FName{}).Set("AmmoRowName", ue::FName{});
            e.Invoke();
        }
    }
    // A body that is already a ragdoll gets shoved, like the kick does.
    bool shoved = false;
    if (c.comp && Live(c.comp)) {
        ue::Call sim(c.comp, "IsSimulatingPhysics");
        if (sim.Valid() && HasParam(sim, "BoneName", sizeof(ue::FName))) {
            sim.Set("BoneName", c.bone);
            if (sim.Invoke() && sim.Result<bool>()) {
                ue::Call push(c.comp, "AddImpulseAtLocation");
                push.Set("Impulse", dir * ((g.pistol ? S.pistol_push : S.long_push) * swing))
                    .Set("Location", c.point).Set("BoneName", c.bone);
                shoved = push.Invoke();
            }
        }
    }
    Feedback(player, c, dir, swing);
    input::Haptic(static_cast<input::Hand>(g.hand), static_cast<float>(0.6 + 0.4 * swing), 0.07f);
    if (g.second_hand == 0 || g.second_hand == 1)
        input::Haptic(static_cast<input::Hand>(g.second_hand), static_cast<float>(0.4 + 0.3 * swing), 0.06f);
    double hp_after = -1, max_after = -1;
    const bool after = !dead && GetHp(c.actor, &hp_after, &max_after);
    ++g_counts.hits;
    g_counts.dirty = true;
    log::Info("gun melee: %s %s hit %s %s (%s, part %s, surface %u) at %.1f m/s (%.0f%% swing, found by %s): "
              "damage %.1f (%.0f%% of max HP %.0f x body-part %.2f%s), stun %.1f (kick %.1f x %.2f)%s%s",
              g.pistol ? "pistol" : "long gun", pt.name, TargetKind(c), ue::GetName(c.actor).c_str(),
              ue::GetName(ue::GetClass(c.actor)).c_str(), bone.c_str(), c.surface, speed, swing * 100.0,
              c.found_by, dead ? 0.0 : damage, t.damage_share * swing * 100.0, has_hp ? max_hp : 100.0, dmul,
              armor > 0 ? ", armour" : "", dead ? 0.0 : stun, kick_stun, t.stun_share * swing,
              dead ? ", already dead" : "", shoved ? ", ragdoll shoved" : "");
    if (has_hp && after) log::Info("gun melee: %s HP %.1f -> %.1f (of %.0f)", ue::GetName(c.actor).c_str(), hp, hp_after, max_after);
    (void)clock;
}

// Something an enemy carries or wears (its weapon, a mask): part of the
// enemy, not a wall between you and it.
bool OnAnEnemy(UObject actor) {
    if (!Live(actor)) return false;
    for (const char* fn : {"GetAttachParentActor", "GetOwner"}) {
        ue::Call c(actor, fn);
        const UObject parent = c.Valid() && c.Invoke() ? c.Result<UObject>() : 0;
        if (!parent) continue;
        for (const Npc& n : g_npcs)
            if (n.actor == parent) return true;
    }
    return false;
}

bool ShouldHit(UObject actor, UObject player) {
    if (!Live(actor) || actor == player) return false;
    return ClassHas(g_hittable_class, ue::GetClass(actor), &HasHitEvent);
}

void LogPoints(int slot, const HeldGun& g, const Track& tr) {
    if (g_logged_points_for[slot] == g.gun) return;
    g_logged_points_for[slot] = g.gun;
    std::string list;
    for (int i = 0; i < tr.n; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s (%.1f %.1f %.1f) r %.1f", i ? ", " : "", tr.pts[i].name,
                      tr.pts[i].local.X, tr.pts[i].local.Y, tr.pts[i].local.Z, tr.pts[i].radius);
        list += buf;
    }
    log::Info("gun melee: %s %s in the %s hand can hit with: %s (cm from the grip)", g.pistol ? "pistol" : "long gun",
              ue::GetName(g.gun).c_str(), g.hand == 0 ? "left" : "right", list.c_str());
}

// The sweeps, in order: the game's hit channel (bullets and the kick), then
// the visibility channel (where the gun's wall check meets the hub's
// mannequins), by the game's hit boxes and then by the shapes you see.
struct Pass {
    uint8_t channel;
    bool complex;
    const char* name;   // for a hit's log line
    const char* tag;    // for a miss's log line
};
constexpr Pass kPasses[] = {
    {kGameHitChannel, false, "the game's hit channel", "hit channel:"},
    {0, false, "the visibility channel", "visibility:"},
    {0, true, "the shapes you see", "shapes you see:"},
};
constexpr int kPassCount = static_cast<int>(sizeof(kPasses) / sizeof(kPasses[0]));
constexpr int kByEnemyBody = 3;

// What a contact is to a swing.
TouchKind KindOf(const Contact& c, UObject player, bool* npc) {
    *npc = false;
    if (!c.actor) return TouchKind::kOther;   // the level itself
    if (c.actor == player) return TouchKind::kPassThrough;
    if (ShouldHit(c.actor, player)) {
        *npc = ClassHas(g_npc_class, ue::GetClass(c.actor), &IsNpcBase);
        // An enemy's capsule (no body part) is not a hit by itself: its body
        // shapes decide (below).
        return *npc && !c.bone.comparison_index ? TouchKind::kPassThrough : TouchKind::kTarget;
    }
    return OnAnEnemy(c.actor) ? TouchKind::kPassThrough : TouchKind::kOther;
}

// What a missed swing's sweeps met, each thing once, for its log line.
void Remember(Track::Swing& sw, const char* tag, const Contact& c, TouchKind kind) {
    if (g_miss_lines >= kMaxMissLines || sw.met.size() >= 8) return;
    const char* what = kind == TouchKind::kTarget         ? "can be hit, but behind a wall"
                       : kind == TouchKind::kPassThrough ? "passed through"
                       : c.blocking                       ? "a wall"
                                                          : "passed through";
    std::string e = std::string(tag) + " " + (c.actor ? ue::GetName(c.actor) : std::string("the level")) + " (" +
                    what + ")";
    if (std::find(sw.met.begin(), sw.met.end(), e) == sw.met.end()) sw.met.push_back(std::move(e));
}

// A swing ended (a frame under hitting speed, or the gun left the hand):
// one log line for a swing that hit nothing.
void FinishSwing(Track& tr) {
    Track::Swing& sw = tr.swing;
    if (sw.frames <= 0) return;
    ++g_counts.swings;
    g_counts.dirty = true;
    if (!sw.hit && g_miss_lines < kMaxMissLines) {
        ++g_miss_lines;
        std::string met;
        for (size_t i = 0; i < sw.met.size(); ++i) met += (i ? "; " : "") + sw.met[i];
        char enemy[48] = "";
        if (sw.nearest_enemy < 1e8) std::snprintf(enemy, sizeof(enemy), "; nearest enemy %.0f cm", sw.nearest_enemy);
        log::Info("gun melee: %s %s swing hit nothing: %d frame(s) at hitting speed, top %.1f m/s; the sweeps met %s%s%s",
                  sw.pistol ? "pistol" : "long gun", sw.part, sw.frames, sw.top,
                  met.empty() ? "nothing at all" : met.c_str(), enemy,
                  g_miss_lines == kMaxMissLines ? " (missed swings stop being logged here)" : "");
    }
    sw = Track::Swing{};
}

void UpdateGun(int slot, const Frame& f, const HeldGun& g) {
    Track& tr = g_track[slot];
    if (tr.gun != g.gun || tr.hand != g.hand || tr.pistol != g.pistol || tr.frame + 1 != f.frame) {
        FinishSwing(tr);
        tr.have = false;
    }
    if (tr.have && f.xr_time == tr.xr_time) { tr.frame = f.frame; return; }   // no new tracking: keep the last
    const bool fresh = !tr.have;
    tr.gun = g.gun;
    tr.hand = g.hand;
    tr.pistol = g.pistol;
    tr.n = StrikePoints(g.pistol ? GunKind::kPistol : GunKind::kLongGun, g.muzzle_dist, g.bore_height, g.have_stock,
                        g.stock, tr.pts);
    LogPoints(slot, g, tr);
    const FVector ct = XrToUePosition(g.hand_tracking.position.x, g.hand_tracking.position.y,
                                      g.hand_tracking.position.z, 100.0);
    const FQuat cq = XrToUeQuat(g.hand_tracking.orientation.x, g.hand_tracking.orientation.y,
                                g.hand_tracking.orientation.z, g.hand_tracking.orientation.w);
    const double scale = stereo::TrackingScale();
    // The hand's own path: the wall check holds the drawn gun out of
    // whatever it reaches (a mannequin, an enemy), which would stop the swing
    // a frame early and read as slow. Walls still stop a hit (below).
    const FVector grip = HandGrip(g.grip, g.wall_push);
    FVector now_w[kMaxPoints], now_t[kMaxPoints];
    for (int i = 0; i < tr.n; ++i) {
        now_w[i] = grip + g.frame.Rotate(tr.pts[i].local);
        now_t[i] = TrackingPoint(now_w[i], g.hand_world.location, g.hand_world.rotation, ct, cq, scale);
    }
    const double dt = tr.have ? static_cast<double>(f.xr_time - tr.xr_time) * 1e-9 : 0.0;
    // The first moments in a hand are the grab settling, not a swing.
    const bool settled = tr.have && f.clock >= tr.since && f.clock - tr.since >= kSettleS;
    const bool swing_ok = settled && !g.kicking;
    const Tuning& t = g.pistol ? S.pistol : S.long_gun;
    double fastest = 0;
    int fastest_i = -1;
    for (int i = 0; swing_ok && i < tr.n; ++i) {
        const double v = Speed(tr.prev_t[i], now_t[i], dt);
        if (v > fastest) { fastest = v; fastest_i = i; }
    }
    bool fast = fastest_i >= 0 && fastest >= t.min_speed;
    if (fast && fastest > kMaxSwingSpeed) {   // the gun jumped: not a swing
        ++g_counts.jumps;
        g_counts.dirty = true;
        fast = false;
    }
    if (!fast) FinishSwing(tr);
    if (fast) {
        ++g_counts.fast;
        Track::Swing& sw = tr.swing;
        if (!sw.frames) {
            sw.t0 = f.clock;
            sw.pistol = g.pistol;
            sw.part = tr.pts[fastest_i].name;
        }
        ++sw.frames;
        sw.top = std::max(sw.top, fastest);
        bool hit_any = false;
        for (int i = 0; i < tr.n && !hit_any; ++i) {
            const double v = Speed(tr.prev_t[i], now_t[i], dt);
            if (v < t.min_speed || v > kMaxSwingSpeed) continue;
            const FVector a = tr.prev_w[i], b = now_w[i];
            const FVector move = b - a;
            const double len = Length(move);
            if (len < 0.2 || len > 80.0) continue;   // a jump (teleport, recentre): not a swing
            const FVector dir = move * (1.0 / len);
            const double r = tr.pts[i].radius;
            Contact c;
            int by = -1;
            // 1) Sweeps through the world (mannequins, targets, enemies whose
            // bodies answer to them). Anything that stops a sweep and cannot
            // be hit is a wall: a target only counts on this side of it.
            double wall_game = 1e9;   // a wall on the game's hit channel: enemies count only on this side of it
            for (int p = 0; p < kPassCount && !c.actor; ++p) {
                Contact got[kMaxTouches];
                Touch touch[kMaxTouches];
                bool npc[kMaxTouches] = {};
                const int n = Sweeper()->Run(f.player, a, b, r, kPasses[p].channel, kPasses[p].complex, f.ignore, got,
                                             kMaxTouches);
                for (int k = 0; k < n; ++k) {
                    touch[k].kind = KindOf(got[k], f.player, &npc[k]);
                    touch[k].blocking = got[k].blocking;
                    touch[k].dist = got[k].dist;
                }
                double wall_at = 1e9;
                const int pick = PickTouch(touch, n, wall_game, r, &wall_at);
                if (p == 0) wall_game = wall_at;
                if (pick >= 0) {
                    c = got[pick];
                    c.npc = npc[pick];
                    c.found_by = kPasses[p].name;
                    by = p;
                } else {
                    for (int k = 0; k < n && k < 3; ++k) Remember(sw, kPasses[p].tag, got[k], touch[k].kind);
                }
            }
            // 2) Enemies' own body shapes (an enemy's collision may let the
            // game's channels pass through it).
            for (const Npc& n : g_npcs) {
                if (c.actor) break;
                if (!Live(n.actor)) continue;
                const double away = Length(ActorLoc(n.actor) - b);
                sw.nearest_enemy = std::min(sw.nearest_enemy, away);
                if (away > 220.0) continue;
                UObject meshes[2] = {0, 0};
                ue::GetProp(n.actor, "Mesh", meshes[0]);
                ue::GetProp(n.actor, "BodyMesh", meshes[1]);
                for (UObject m : meshes) {
                    if (!m || (m == meshes[1] && meshes[0] == meshes[1])) continue;
                    Contact e;
                    if (BodySweep(m, a, b, r, &e) && Length(e.point - a) <= wall_game + r) {
                        c = e;
                        c.actor = n.actor;
                        c.npc = true;
                        c.found_by = m == meshes[0] ? "the enemy's body (Mesh)" : "the enemy's body (BodyMesh)";
                        by = kByEnemyBody;
                        break;
                    }
                }
            }
            if (!c.actor) continue;
            sw.hit = true;
            hit_any = true;
            if (!g_cool.Ready(c.actor, f.clock, t.cooldown_s)) break;   // the same swing, still in the body
            g_cool.Mark(c.actor, f.clock);
            if (by >= 0 && by < 4) ++g_counts.by[by];
            Apply(f.player, g, tr.pts[i], v, dir, c, f.clock);
        }
    }
    for (int i = 0; i < tr.n; ++i) {
        tr.prev_w[i] = now_w[i];
        tr.prev_t[i] = now_t[i];
    }
    tr.xr_time = f.xr_time;
    tr.frame = f.frame;
    if (fresh) tr.since = f.clock;
    tr.have = true;
}

void OnSettingsChanged() {
    const bool was = S.on;
    LoadSettings();
    if (was != S.on) log::Info("gun melee: %s", S.on ? "on" : "off");
}

}  // namespace

void Init() {
    LoadSettings();
    config::AddChangeListener(&OnSettingsChanged);
    perf::Register(&g_perf);
    log::Info("gun melee: %s (a hit needs %.1f m/s, full at %.1f m/s; pistol %.0f%% of max HP, long gun %.0f%%)",
              S.on ? "on" : "off", S.pistol.min_speed, S.pistol.full_speed, S.pistol.damage_share * 100.0,
              S.long_gun.damage_share * 100.0);
}

namespace {
Stance g_stance;
uintptr_t g_stance_gun = 0;
int g_stance_lines = 0;
}  // namespace

void ButtStrikeStanceReset() {
    if (g_stance.On() && g_stance_lines < 40) {
        ++g_stance_lines;
        log::Info("gun melee: butt-strike stance off (the long gun left the hand)");
    }
    g_stance.Reset();
    g_stance_gun = 0;
}

bool ButtStrikeStance(uintptr_t gun, const hands::Pose& main, const hands::Pose& off, const hands::Pose& head,
                      const FQuat& gun_frame, bool tracked, double clock) {
    if (!S.stance_on || !gun) {
        ButtStrikeStanceReset();
        return false;
    }
    if (gun != g_stance_gun) {
        g_stance.Reset();
        g_stance_gun = gun;
    }
    // Real distances: the world can be scaled (world scale).
    double scale = stereo::TrackingScale();
    if (!(scale > 1e-3)) scale = 1.0;
    StanceInput in;
    in.valid = tracked;
    in.main = main.location * (1.0 / scale);
    in.off = off.location * (1.0 / scale);
    in.eyes = head.location * (1.0 / scale);
    in.view = head.rotation.Rotate(FVector{1, 0, 0});
    in.barrel = gun_frame.Rotate(FVector{1, 0, 0});
    in.now = clock;
    const bool was = g_stance.On();
    const bool on = g_stance.Update(in, S.stance);
    if (on != was && g_stance_lines < 40) {
        ++g_stance_lines;
        log::Info("gun melee: butt-strike stance %s (main hand %.0f cm from the eyes, off hand %.0f cm above it, "
                  "barrel %.0f deg off your view; in at <= %.0f cm, >= %.0f cm, >= %.0f deg)%s",
                  on ? "on: the main hand takes its butt-strike pose" : "off", g_stance.head_cm, g_stance.above_cm,
                  g_stance.view_deg, S.stance.near_head_cm, S.stance.off_above_cm, S.stance.off_view_deg,
                  g_stance_lines == 40 ? " (stance changes stop being logged here)" : "");
    }
    return on;
}

void Update(const Frame& f) {
    if (!S.on || !f.player || f.count <= 0) {
        for (Track& tr : g_track) FinishSwing(tr);
        return;
    }
    perf::Scope timed(g_perf);
    if (!L.Ready()) return;
    if (g_classes_player != f.player) {
        // A new level (new player): its classes and bodies are new too.
        g_classes_player = f.player;
        g_hittable_class.clear();
        g_npc_class.clear();
        g_bone_surfaces.clear();
        g_npcs.clear();
        g_npcs_at = -1e9;
        g_cool.Clear();
    }
    RefreshNpcs(f.player, f.clock);
    for (int i = 0; i < 2; ++i) {
        if (i < f.count && f.guns[i].gun) UpdateGun(i, f, f.guns[i]);
        else FinishSwing(g_track[i]);
    }
    if (g_counts.dirty && (f.clock >= g_counts.next_log || f.clock + 120.0 < g_counts.next_log)) {
        g_counts.dirty = false;
        g_counts.next_log = f.clock + 60.0;
        log::Info("gun melee: so far %d swings (%d frames at hitting speed), %d hits (found by the game's hit "
                  "channel %d, the visibility channel %d, the shapes you see %d, an enemy's body %d); %d jumps of "
                  "the gun over %.0f m/s not counted as swings",
                  g_counts.swings, g_counts.fast, g_counts.hits, g_counts.by[0], g_counts.by[1], g_counts.by[2],
                  g_counts.by[3], g_counts.jumps, kMaxSwingSpeed);
    }
}

}  // namespace holevr::vr::gun_melee
