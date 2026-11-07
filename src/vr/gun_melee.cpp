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
    const char* found_by = "";
};

// Copies a hit result into BreakHitResult/GetSurfaceType and reads it.
struct HitReader {
    ue::Call unpack{L.gps, "BreakHitResult"};
    ue::Call surface{L.gps, "GetSurfaceType"};
    int32_t size = 0;
    bool ok = false;
    HitReader() {
        int32_t s2 = 0;
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
        surface.Set("ReturnValue", uint8_t{0});
        c->surface = surface.Invoke() ? surface.Result<uint8_t>() : 0;
        return true;
    }
};
HitReader* Reader() {
    static HitReader* r = new HitReader();
    return r->ok ? r : nullptr;
}

// The game's hit channel, swept from where the strike point was to where it
// is: finds whatever it touches first (a wall stops it). Hits only count on
// things with the game's hit event.
struct WorldSweep {
    ue::Call t{L.ksl, "SphereTraceSingle"};
    int32_t radius_size = 0, hit_size = 0;
    bool ok = false;
    WorldSweep() {
        ok = t.Valid() && t.Raw("Radius", &radius_size) && (radius_size == 4 || radius_size == 8) &&
             t.Raw("OutHit", &hit_size) && hit_size > 0 && HasParam(t, "Start", sizeof(FVector)) &&
             HasParam(t, "End", sizeof(FVector)) && HasParam(t, "TraceChannel", 1) &&
             HasParam(t, "ActorsToIgnore", 16) && HasParam(t, "bIgnoreSelf", sizeof(bool)) &&
             HasParam(t, "ReturnValue", sizeof(bool));
        if (!ok) log::Warn("gun melee: SphereTraceSingle unusable; only enemies' own shapes are checked");
    }
    bool Run(UObject player, const FVector& a, FVector b, double r, const std::vector<UObject>& ignore, Contact* c) {
        if (!ok) return false;
        if (Length(b - a) < 1e-3) b = a + FVector{0.01, 0, 0};
        struct { const UObject* data; int32_t num, max; } arr{ignore.data(), static_cast<int32_t>(ignore.size()),
                                                             static_cast<int32_t>(ignore.size())};
        t.Set("WorldContextObject", player).Set("Start", a).Set("End", b).Set("TraceChannel", kGameHitChannel)
            .Set("bTraceComplex", false).Set("ActorsToIgnore", arr).Set("DrawDebugType", uint8_t{0})
            .Set("bIgnoreSelf", true).Set("ReturnValue", false);
        if (radius_size == 4) t.Set("Radius", static_cast<float>(r));
        else t.Set("Radius", r);
        if (!t.Invoke() || !t.Result<bool>()) return false;
        int32_t n = 0;
        const uint8_t* hit = t.Raw("OutHit", &n);
        HitReader* rd = Reader();
        return rd && rd->Read(hit, n, c);
    }
};
WorldSweep* Sweeper() {
    static WorldSweep* w = new WorldSweep();
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
bool GetHp(UObject target, double* hp, double* max_hp) {
    ue::Call c(target, "GetHP");
    if (!c.Valid() || !HasParam(c, "HP", sizeof(double)) || !c.Invoke()) return false;
    *hp = c.Get<double>("HP", -1.0);
    *max_hp = c.Get<double>("MaxHP", -1.0);
    return std::isfinite(*hp) && std::isfinite(*max_hp);
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

void Feedback(UObject player, const Contact& c, const FVector& dir, double swing) {
    if (L.gps) {
        if (const UObject sound = g_hit_sound.Get()) {
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
};
Track g_track[2];
Cooldowns g_cool;
UObject g_logged_points_for[2] = {0, 0};

// Session counts for the log.
struct Counts { int fast = 0, hits = 0; double next_log = 0; } g_counts;
int g_fast_lines = 0;

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
    const bool dead = has_hp && hp <= 0;
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

void UpdateGun(int slot, const Frame& f, const HeldGun& g) {
    Track& tr = g_track[slot];
    if (tr.gun != g.gun || tr.hand != g.hand || tr.pistol != g.pistol || tr.frame + 1 != f.frame) tr.have = false;
    if (tr.have && f.xr_time == tr.xr_time) { tr.frame = f.frame; return; }   // no new tracking: keep the last
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
    FVector now_w[kMaxPoints], now_t[kMaxPoints];
    for (int i = 0; i < tr.n; ++i) {
        now_w[i] = g.grip + g.frame.Rotate(tr.pts[i].local);
        now_t[i] = TrackingPoint(now_w[i], g.hand_world.location, g.hand_world.rotation, ct, cq, scale);
    }
    const double dt = tr.have ? static_cast<double>(f.xr_time - tr.xr_time) * 1e-9 : 0.0;
    const bool swing_ok = tr.have && !g.kicking;
    const Tuning& t = g.pistol ? S.pistol : S.long_gun;
    double fastest = 0;
    int fastest_i = -1;
    for (int i = 0; swing_ok && i < tr.n; ++i) {
        const double v = Speed(tr.prev_t[i], now_t[i], dt);
        if (v > fastest) { fastest = v; fastest_i = i; }
    }
    if (fastest_i >= 0 && fastest >= t.min_speed) {
        ++g_counts.fast;
        bool hit_any = false;
        const std::vector<UObject>& ignore = f.ignore;
        for (int i = 0; i < tr.n && !hit_any; ++i) {
            const double v = Speed(tr.prev_t[i], now_t[i], dt);
            if (v < t.min_speed) continue;
            const FVector a = tr.prev_w[i], b = now_w[i];
            const FVector move = b - a;
            const double len = Length(move);
            if (len < 0.2 || len > 80.0) continue;   // a jump (teleport, recentre): not a swing
            const FVector dir = move * (1.0 / len);
            Contact c;
            // 1) The game's hit channel: mannequins, targets, and enemies when their
            // bodies answer to it. A wall in the way stops the swing.
            // An enemy's capsule (no body part) is not a hit by itself: its body
            // shapes below decide.
            const bool world_hit = Sweeper()->Run(f.player, a, b, tr.pts[i].radius, ignore, &c) &&
                                   ShouldHit(c.actor, f.player);
            if (world_hit) c.npc = ClassHas(g_npc_class, ue::GetClass(c.actor), &IsNpcBase);
            if (world_hit && !(c.npc && !c.bone.comparison_index)) {
                c.found_by = "the game's hit channel";
            } else {
                // 2) Enemies' own body shapes (an enemy's collision may let the
                // game's channel pass through it).
                c = Contact{};
                for (const Npc& n : g_npcs) {
                    if (!Live(n.actor)) continue;
                    if (Length(ActorLoc(n.actor) - b) > 220.0) continue;
                    UObject meshes[2] = {0, 0};
                    ue::GetProp(n.actor, "Mesh", meshes[0]);
                    ue::GetProp(n.actor, "BodyMesh", meshes[1]);
                    for (UObject m : meshes) {
                        if (!m || (m == meshes[1] && meshes[0] == meshes[1])) continue;
                        if (BodySweep(m, a, b, tr.pts[i].radius, &c)) {
                            c.actor = n.actor;
                            c.npc = true;
                            c.found_by = m == meshes[0] ? "the enemy's body (Mesh)" : "the enemy's body (BodyMesh)";
                            break;
                        }
                        c = Contact{};
                    }
                    if (c.actor) break;
                }
            }
            if (!c.actor) continue;
            if (!g_cool.Ready(c.actor, f.clock, t.cooldown_s)) { hit_any = true; break; }
            g_cool.Mark(c.actor, f.clock);
            Apply(f.player, g, tr.pts[i], v, dir, c, f.clock);
            hit_any = true;
        }
        if (!hit_any && g_fast_lines < 12) {
            ++g_fast_lines;
            log::Info("gun melee: %s %s swung at %.1f m/s, touched nothing it can hit%s",
                      g.pistol ? "pistol" : "long gun", tr.pts[fastest_i].name, fastest,
                      g_fast_lines == 12 ? " (swings without a hit stop being logged here)" : "");
        }
    }
    for (int i = 0; i < tr.n; ++i) {
        tr.prev_w[i] = now_w[i];
        tr.prev_t[i] = now_t[i];
    }
    tr.xr_time = f.xr_time;
    tr.frame = f.frame;
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

void Update(const Frame& f) {
    if (!S.on || !f.player || f.count <= 0) return;
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
    for (int i = 0; i < f.count && i < 2; ++i)
        if (f.guns[i].gun) UpdateGun(i, f, f.guns[i]);
    if (f.clock >= g_counts.next_log && (g_counts.fast || g_counts.hits)) {
        g_counts.next_log = f.clock + 60.0;
        log::Info("gun melee: %d fast swing frames, %d hits so far", g_counts.fast, g_counts.hits);
    }
}

}  // namespace holevr::vr::gun_melee
