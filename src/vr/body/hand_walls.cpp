#include "vr/body/hand_walls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/perf.h"
#include "ue/call.h"
#include "vr/wall_shapes.h"
#include "vr/world_menu.h"

namespace holevr::vr::body::hand_walls {
namespace {
using namespace ik;
struct Settings {
    bool on = true, legacy_probe = false;
    wall_shapes::Mode shapes = wall_shapes::Mode::kNearest;
    double probe = 14.0;
    hand_motion::Settings motion;
} S;
constexpr double kProbeRadius = 2.0;
struct TArrayRaw { const UObject* data; int32_t num, max; };
struct Kept { UObject obj = 0; int32_t index = -1; };
bool Alive(const Kept& k) {
    return k.obj && k.index >= 0 && mem::ReadOr<int32_t>(k.obj + ue::off::kObjIndex, -1) == k.index &&
           ue::objects::Get(k.index) == k.obj;
}
Kept Keep(UObject object) { return {object, mem::ReadOr<int32_t>(object + ue::off::kObjIndex, -1)}; }
struct Side {
    hand_motion::State motion;
    hand_motion::Result result;
    bool ahead = false, have_plane = false;
    FVector plane_point, plane_normal;
    double plane_ms = 0;
    Kept plane_actor;
    int frame_queries = 0;
    bool verified = false;
    double checked_ms = 0;
    struct Support { hand_motion::RootSupport plane; double expires_ms=0; };
    std::array<Support,80> supports{};
    int support_count=0;
};
constexpr int kFrameQueryBudget = 32;
Side g_side[2];
hand_motion::PalmShape g_palm[2];
perf::Counter g_perf{"body hand proxy collision"};
UObject g_ksl = 0, g_gps = 0;
Kept g_player;
double g_next_lookup_ms = 0;
std::vector<Kept> g_attached;
std::vector<UObject> g_ignore;
std::vector<UObject> g_own_gear;
double g_attached_age = 1e9;
bool g_announced = false;
double g_log_age = 0;
unsigned g_log_queries = 0, g_log_blocked = 0, g_log_recovery = 0;
unsigned g_log_budget = 0, g_log_rotation_blocked = 0, g_log_max_queries = 0;
double g_log_error = 0;

int ActiveSupports(const Side& side, std::array<hand_motion::RootSupport,80>& out) {
    const double now=perf::NowMs(); int count=0;
    for(int i=0;i<side.support_count;++i) {
        const auto& cached=side.supports[i];
        const double remaining=(cached.expires_ms-now)/1000.0;
        if(remaining<=0) continue;
        out[count]=cached.plane; out[count].remaining_seconds=std::min(remaining,.05); ++count;
    }
    return count;
}

void RefreshAttached(UObject player, double dt) {
    g_attached_age += dt;
    if (g_attached_age < 0.5) return;
    g_attached_age = 0;
    g_attached.clear();
    ue::Call c(player, "GetAttachedActors");
    if (!c.Valid()) return;
    int32_t flag = 0;
    if (c.Raw("bResetArray", &flag) && flag == 1) c.Set("bResetArray", true);
    if (c.Raw("bRecursivelyIncludeAttachedActors", &flag) && flag == 1) c.Set("bRecursivelyIncludeAttachedActors", true);
    if (!c.Invoke()) return;
    int32_t size = 0;
    const uint8_t* raw = c.Raw("OutActors", &size);
    struct Arr { uintptr_t data; int32_t num, max; } arr{};
    if (!raw || size < static_cast<int32_t>(sizeof(Arr))) return;
    std::memcpy(&arr, raw, sizeof(Arr));
    if (arr.num < 0 || arr.num > arr.max || arr.num > 4096) return;
    for (int i = 0; i < arr.num && i < 64; ++i) {
        UObject a = mem::ReadOr<uintptr_t>(arr.data + i * sizeof(uintptr_t), 0);
        if (a) g_attached.push_back(Keep(a));
    }
}

bool Has(const ue::Call& call, const char* parameter, size_t expected) {
    int32_t size = 0;
    return call.Raw(parameter, &size) && size == static_cast<int32_t>(expected);
}

// Reuse reflected parameter buffers for this body's frame. Validate all
// fields before any query: Call::Set/Get intentionally tolerate mismatches,
// but a missing boolean return must never masquerade as clear space here.
// No hardcoded FHitResult offsets or engine-allocated output arrays are used.
struct TraceAdapter {
    ue::Call trace{g_ksl, "SphereTraceSingle"};
    ue::Call box{g_ksl, "BoxTraceSingle"};
    ue::Call unpack{g_gps, "BreakHitResult"};
    int32_t radius_size = 0, hit_size = 0;
    bool usable = false, box_usable = false;

    explicit TraceAdapter(UObject player) {
        if (!trace.Valid() || !unpack.Valid()) return;
        trace.Raw("Radius", &radius_size);
        int32_t input_size = 0;
        if ((radius_size != 4 && radius_size != 8) ||
            !trace.Raw("OutHit", &hit_size) || hit_size <= 0 ||
            !unpack.Raw("Hit", &input_size) || input_size != hit_size ||
            !Has(trace, "WorldContextObject", sizeof(UObject)) ||
            !Has(trace, "Start", sizeof(FVector)) || !Has(trace, "End", sizeof(FVector)) ||
            !Has(trace, "TraceChannel", sizeof(uint8_t)) ||
            !Has(trace, "bTraceComplex", sizeof(bool)) ||
            !Has(trace, "ActorsToIgnore", sizeof(TArrayRaw)) ||
            !Has(trace, "bIgnoreSelf", sizeof(bool)) || !Has(trace, "ReturnValue", sizeof(bool)) ||
            !Has(unpack, "bBlockingHit", sizeof(bool)) || !Has(unpack, "bInitialOverlap", sizeof(bool)) ||
            !Has(unpack, "Location", sizeof(FVector)) || !Has(unpack, "ImpactPoint", sizeof(FVector)) ||
            !Has(unpack, "ImpactNormal", sizeof(FVector)) || !Has(unpack, "Normal", sizeof(FVector)) ||
            !Has(unpack, "HitActor", sizeof(UObject))) return;
        TArrayRaw ignore{g_ignore.data(), static_cast<int32_t>(g_ignore.size()), static_cast<int32_t>(g_ignore.size())};
        trace.Set("WorldContextObject", player).Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false)
            .Set("ActorsToIgnore", ignore).Set("bIgnoreSelf", true);
        usable = true;
        int32_t box_hit_size = 0;
        box_usable = box.Valid() && box.Raw("OutHit", &box_hit_size) && box_hit_size == hit_size &&
            Has(box, "WorldContextObject", sizeof(UObject)) && Has(box, "Start", sizeof(FVector)) &&
            Has(box, "End", sizeof(FVector)) && Has(box, "HalfSize", sizeof(FVector)) &&
            Has(box, "Orientation", sizeof(FRotator)) && Has(box, "TraceChannel", sizeof(uint8_t)) &&
            Has(box, "bTraceComplex", sizeof(bool)) && Has(box, "ActorsToIgnore", sizeof(TArrayRaw)) &&
            Has(box, "bIgnoreSelf", sizeof(bool)) && Has(box, "ReturnValue", sizeof(bool));
        if (box_usable)
            box.Set("WorldContextObject", player).Set("TraceChannel", uint8_t{0}).Set("bTraceComplex", false)
                .Set("ActorsToIgnore", ignore).Set("bIgnoreSelf", true);
        static bool warned = false;
        if (!box_usable && !warned) log::Warn("hand palm: BoxTraceSingle reflection unavailable; palm queries fail closed");
        warned = !box_usable;
    }

    hand_motion::Sweep Run(ue::Call& call, Kept* actor) {
        hand_motion::Sweep hit;
        if (actor) *actor = {};
        // The final-wrist guard reuses this adapter after finger queries have
        // refreshed the shared ignore list. Rebind its current live extent;
        // retaining the constructor's count could include an erased actor.
        const auto& live_ignore = IgnoredActors();
        TArrayRaw ignore{live_ignore.data(), static_cast<int32_t>(live_ignore.size()),
                         static_cast<int32_t>(live_ignore.size())};
        call.Set("ActorsToIgnore", ignore);
        call.Set("ReturnValue", false);
        if (!call.Invoke()) return hit;
        hit.available = true;
        if (!call.Result<bool>()) return hit;
        int32_t source_size = 0, target_size = 0;
        const uint8_t* raw = call.Raw("OutHit", &source_size);
        uint8_t* input = unpack.Raw("Hit", &target_size);
        if (!raw || !input || source_size != hit_size || target_size != hit_size) return {};
        std::memcpy(input, raw, static_cast<size_t>(hit_size));
        if (!unpack.Invoke()) return {};
        hit.hit = unpack.Get<bool>("bBlockingHit");
        if (!hit.hit) return hit;
        const Kept contact = Keep(unpack.Get<UObject>("HitActor"));
        if (contact.obj && (!Alive(contact) ||
            std::find(g_ignore.begin(), g_ignore.end(), contact.obj) != g_ignore.end())) return {};
        hit.initial_overlap = unpack.Get<bool>("bInitialOverlap");
        hit.centre = unpack.Get<FVector>("Location");
        hit.point = unpack.Get<FVector>("ImpactPoint");
        hit.normal = unpack.Get<FVector>(hit.initial_overlap ? "Normal" : "ImpactNormal");
        if (actor) *actor = contact;
        return hit;
    }
    // The query against the shapes the wall_shape setting names; with both,
    // the nearest hit wins (an overlap at the start first), so the hand goes
    // into neither the surface you see nor the game's hit box. Either query
    // failing fails the whole query (closed).
    hand_motion::Sweep Shapes(ue::Call& call, const FVector& from, Kept* actor) {
        hand_motion::Sweep best;
        Kept best_actor;
        const int passes = wall_shapes::Passes(S.shapes);
        for (int pass = 0; pass < passes; ++pass) {
            call.Set("bTraceComplex", wall_shapes::Complex(S.shapes, pass));
            Kept got;
            const hand_motion::Sweep hit = Run(call, &got);
            if (!hit.available) {
                if (actor) *actor = {};
                return hit;
            }
            bool better = pass == 0 || (hit.hit && !best.hit);
            if (!better && hit.hit && best.hit && !best.initial_overlap)
                better = hit.initial_overlap || Length(hit.centre - from) < Length(best.centre - from);
            if (better) best = hit, best_actor = got;
        }
        if (actor) *actor = best_actor;
        return best;
    }
    hand_motion::Sweep Trace(const FVector& from, FVector to, double radius, Kept* actor = nullptr) {
        if (!usable) return {};
        if (Length(to - from) < 1e-5) to = from + FVector{0.001, 0, 0};
        trace.Set("Start", from).Set("End", to);
        if (radius_size == 4) trace.Set("Radius", static_cast<float>(radius));
        else trace.Set("Radius", radius);
        return Shapes(trace, from, actor);
    }
    hand_motion::Sweep Box(const FVector& from, FVector to, const FVector& half, const FQuat& rotation,
                           Kept* actor = nullptr) {
        if (!box_usable) return {};
        if (Length(to - from) < 1e-5) to = from + FVector{0.001, 0, 0};
        box.Set("Start", from).Set("End", to).Set("HalfSize", half).Set("Orientation", ToRotator(rotation));
        return Shapes(box, from, actor);
    }
};
std::optional<TraceAdapter> g_backend;
void Remember(Side& side, const FVector& point, const FVector& normal, const Kept& actor) {
    if (Length(normal) < 0.5) return;
    side.have_plane = true;
    side.plane_point = point;
    side.plane_normal = Normalize(normal);
    side.plane_actor = actor;
    side.plane_ms = perf::NowMs();
}
double Setting(const char* name, double fallback, double lo, double hi) {
    const double value = config::GetFloat(name, static_cast<float>(fallback));
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}
}

void Reset(int side) {
    if (side >= 0 && side < 2) { g_side[side] = {}; return; }
    g_side[0] = g_side[1] = {};
    g_ignore.clear();
    g_attached.clear();
    g_attached_age = 1e9;
    g_player = {};
    g_backend.reset();
}

void LoadSettings() {
    S.on = config::GetBool("hand_wall_collision", true);
    // The shapes you see and the game's hit boxes, whichever is nearer, unless
    // wall_shape says otherwise (owner 2026-11-07: hands sank into the hub's
    // walls; vr/wall_shapes.h).
    S.shapes = wall_shapes::ModeSetting();
    S.legacy_probe = !config::GetBool("hand_individual_finger_collision", true) ||
                     !config::GetBool("hand_fingers_rest_on_walls", true);
    auto& m = S.motion;
    m.radius_cm = Setting("hand_wall_radius_cm", 4.0, 1.0, 10.0);
    S.probe = Setting("hand_wall_probe_cm", 14.0, 0.0, 30.0);
    m.frequency_hz = Setting("hand_follow_hz", 10.0, 3.0, 18.0);
    m.damping_ratio = Setting("hand_follow_damping", 1.0, 0.7, 2.0);
    m.max_force_n = Setting("hand_follow_max_force_n", 65.0, 5.0, 150.0);
    m.max_speed_cm_s = Setting("hand_follow_max_speed_cm_s", 700.0, 100.0, 1200.0);
    m.friction_per_s = Setting("hand_surface_friction", 5.0, 0.0, 30.0);
    m.recovery_distance_cm = Setting("hand_recovery_distance_cm", 40.0, 15.0, 100.0);
    Reset();
}

void Apply(UObject player, const hands::Pose& head, hands::HandState hs[2], const hands::HandGrab grab[2], double dt) {
    static bool registered = [] { perf::Register(&g_perf); return true; }();
    (void)registered;
    if (g_announced != S.on) {
        g_announced = S.on;
        log::Info("hand proxy: %s (calibrated palm box, %.1f cm sphere fallback, checked rotation, %.0f N virtual force cap, "
                  "against %s)",
                  S.on ? "on" : "off", S.motion.radius_cm, S.motion.max_force_n,
                  wall_shapes::ModeName(S.shapes));
    }
    if (!player || !ue::IsLive(player)) { Reset(); return; }
    if (g_player.obj != player || !Alive(g_player)) {
        Reset();
        g_player = Keep(player);
        g_ksl = g_gps = 0;
        g_next_lookup_ms = 0;
    }
    const double elapsed = std::isfinite(dt) ? std::clamp(dt, 0.0, 0.25) : 0.0;
    RefreshAttached(player, elapsed);
    g_ignore.clear();
    g_ignore.push_back(player);
    for (const Kept& kept : g_attached) if (Alive(kept)) g_ignore.push_back(kept.obj);
    for (int i = 0; i < 2; ++i) if (grab[i].object && ue::IsLive(grab[i].object)) g_ignore.push_back(grab[i].object);
    if (UObject holder = world_menu::PanelHolder()) g_ignore.push_back(holder);
    for (UObject a : g_own_gear)
        if (ue::IsLive(a) && std::find(g_ignore.begin(), g_ignore.end(), a) == g_ignore.end()) g_ignore.push_back(a);
    if (!S.on) { g_side[0] = g_side[1] = {}; return; }
    perf::Scope timer(g_perf);
    if ((!g_ksl || !g_gps) && perf::NowMs() >= g_next_lookup_ms) {
        g_next_lookup_ms = perf::NowMs() + 5000.0;
        if (!g_ksl) g_ksl = ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!g_gps) g_gps = ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
        if (!g_ksl || !g_gps) log::Warn("hand proxy: reflected trace functions unavailable");
    }
    // An unavailable backend follows Step's unavailable-query policy too:
    // preserve an acquired position, or mark this local body hand invalid.
    g_backend.emplace(player);
    TraceAdapter& backend = *g_backend;
    for (int side = 0; side < 2; ++side) {
        Side& s = g_side[side];
        s.frame_queries = 0;
        hands::HandState& h = hs[side];
        const auto ownership = hands::GetInteraction(static_cast<hands::Hand>(side));
        if (!h.valid || grab[side].kind != hands::GrabKind::kNone ||
            ownership.owner != hands::GripOwner::kNone || !ownership.actions_allowed ||
            hands::EditorHand() >= 0) {
            Reset(side);
            continue;
        }
        // Never keep the old plane just because the hand stayed near it.
        s.have_plane = false;
        s.ahead = false;
        Kept contact_actor;
        const FVector raw_position = h.grip.location;
        const FQuat raw_rotation = Normalized(h.grip.rotation);
        hand_motion::Input input;
        input.target = raw_position;
        input.rotation = raw_rotation;
        input.anchor = head.location;
        input.dt = dt;
        input.palm = g_palm[side];
        std::array<hand_motion::RootSupport,80> supports;
        input.support_count=ActiveSupports(s,supports); input.supports=supports.data();
        s.result = hand_motion::Step(s.motion, S.motion, input,
            [&](const FVector& from, const FVector& to, double radius) {
                Kept actor;
                auto hit = backend.Trace(from, to, radius, &actor);
                if (hit.hit && !hit.initial_overlap) contact_actor = actor;
                return hit;
            }, [&](const FVector& from, const FVector& to, const FVector& half, const FQuat& rotation) {
                Kept actor;
                auto hit = backend.Box(from, to, half, rotation, &actor);
                if (hit.hit && !hit.initial_overlap) contact_actor = actor;
                return hit;
            });
        s.frame_queries = s.result.queries;
        if(s.result.reset) s.support_count=0;
        s.verified = s.result.valid && !s.result.query_failed && !s.result.budget_limited;
        s.checked_ms = perf::NowMs();
        // No verified physical placement (for example an overlapping reset
        // anchor) must not silently fall through to the raw tracked pose.
        // This is only the body copy; controls retain their actual tracking.
        if (!s.result.valid) { h.valid = false; continue; }
        if (s.result.touching) {
            Remember(s, s.result.point, s.result.normal, contact_actor);
            // Where the game's hit box is against what you see, on a line
            // from the eyes through the touched spot (the "wall shapes" log).
            const FVector to = s.result.point - s.result.normal * 3.0;
            if (Length(to - head.location) > 1.0)
                wall_shapes::Compare(player, head.location, to, g_ignore, wall_shapes::kHand);
        }

        const FQuat turn = Normalized(s.result.rotation * raw_rotation.Inverse());
        h.aim.location = s.result.position + turn.Rotate(h.aim.location - raw_position);
        h.wrist.location = s.result.position + turn.Rotate(h.wrist.location - raw_position);
        h.aim.rotation = Normalized(turn * h.aim.rotation);
        h.wrist.rotation = Normalized(turn * h.wrist.rotation);
        h.palm_normal = turn.Rotate(h.palm_normal);
        h.grip.location = s.result.position;
        h.grip.rotation = s.result.rotation;

        // Legacy palm-flattening hint only; individual finger geometry is
        // handled by finger_world later in the body frame.
        if (!s.have_plane && S.legacy_probe && S.probe > 0.0) {
            Kept actor;
            const FVector forward = h.grip.rotation.Rotate(FVector{1,0,0});
            auto hit = backend.Trace(h.grip.location, h.grip.location + forward * S.probe, kProbeRadius, &actor);
            ++s.result.queries;
            ++s.frame_queries;
            if (hit.available && hit.hit && !hit.initial_overlap && Length(hit.normal) > 0.5) {
                s.ahead = true;
                Remember(s, hit.point, hit.normal, actor);
            }
        }
        g_log_queries += s.result.queries;
        g_log_blocked += s.result.touching ? 1 : 0;
        g_log_recovery += s.result.recovering ? 1 : 0;
        g_log_budget += s.result.budget_limited ? 1 : 0;
        g_log_rotation_blocked += s.result.rotation_blocked ? 1 : 0;
        g_log_max_queries = std::max(g_log_max_queries, static_cast<unsigned>(s.result.queries));
        g_log_error = std::max(g_log_error, s.result.error_cm);
    }
    g_log_age += elapsed;
    if (g_log_age >= 10.0) {
        log::Info("hand proxy: %u sweeps (max %u/hand), %u blocked, %u recovery, %u rotation blocked, "
                  "%u budget-limited frames, max tracking separation %.1f cm",
                  g_log_queries, g_log_max_queries, g_log_blocked, g_log_recovery, g_log_rotation_blocked,
                  g_log_budget, g_log_error);
        g_log_age = 0; g_log_queries = g_log_blocked = g_log_recovery = 0; g_log_error = 0;
        g_log_budget = g_log_rotation_blocked = g_log_max_queries = 0;
    }
}

// Only the ones that still exist. The list is made in Apply, which runs only
// while the body is drawn: after a level change (Retry or Home after dying)
// it still held the last level's player and gear, freed with that level, and
// the held gun's wall check gave them to the engine before the body was made
// again: a "fatal error" (owner 2026-11-08 16:12).
const std::vector<UObject>& IgnoredActors() {
    g_ignore.erase(std::remove_if(g_ignore.begin(), g_ignore.end(), [](UObject a) { return !ue::IsLive(a); }),
                   g_ignore.end());
    return g_ignore;
}
void SetOwnGear(const std::vector<UObject>& actors) { g_own_gear = actors; }

hand_motion::Result ConstrainFinal(int side, const hands::Pose& requestedGrip) {
    hand_motion::Result unavailable;
    if (side<0 || side>1 || !S.on || !g_backend || !Alive(g_player)) return unavailable;
    Side& s=g_side[side];
    if (!s.motion.valid || !s.result.valid || perf::NowMs()-s.checked_ms>100.0) return unavailable;
    // Apply already verified this exact pose during the same body frame.
    if (s.verified && Length(requestedGrip.location-s.motion.position)<1e-7 &&
        Length((Normalized(requestedGrip.rotation*s.motion.rotation.Inverse())).Rotate({1,0,0})-FVector{1,0,0})<1e-9 &&
        Length((Normalized(requestedGrip.rotation*s.motion.rotation.Inverse())).Rotate({0,1,0})-FVector{0,1,0})<1e-9) {
        auto same=s.result; same.queries=0; return same;
    }
    hand_motion::Settings settings=S.motion;
    settings.max_queries=std::max(0,kFrameQueryBudget-s.frame_queries);
    Kept actor;
    std::array<hand_motion::RootSupport,80> supports;
    const int support_count=ActiveSupports(s,supports);
    perf::Scope timer(g_perf);
    auto result=hand_motion::Constrain(s.motion,settings,g_palm[side],requestedGrip.location,requestedGrip.rotation,
        [&](const FVector& from,const FVector& to,double radius) {
            Kept hit_actor; auto hit=g_backend->Trace(from,to,radius,&hit_actor);
            if(hit.hit && !hit.initial_overlap) actor=hit_actor;
            return hit;
        },[&](const FVector& from,const FVector& to,const FVector& half,const FQuat& rotation) {
            Kept hit_actor; auto hit=g_backend->Box(from,to,half,rotation,&hit_actor);
            if(hit.hit && !hit.initial_overlap) actor=hit_actor;
            return hit;
        },supports.data(),support_count);
    s.frame_queries+=result.queries;
    g_log_queries+=result.queries;
    g_log_budget+=result.budget_limited?1:0;
    g_log_rotation_blocked+=result.rotation_blocked?1:0;
    g_log_max_queries=std::max(g_log_max_queries,static_cast<unsigned>(s.frame_queries));
    if (result.valid) {
        s.verified=true; s.checked_ms=perf::NowMs();
        // Retain measured pressure only while the same fresh palm plane still
        // supports the checked correction. No stale plane survives failure.
        if(result.touching && s.result.touching && Dot(result.normal,s.result.normal)>.95)
            result.flatness=s.result.flatness;
        s.result=result; s.result.queries=s.frame_queries;
        s.have_plane=false; s.ahead=false;
        if(result.touching) Remember(s,result.point,result.normal,actor);
    } else { s.have_plane=false; s.ahead=false; s.verified=false; }
    return result;
}
void SetRigidSupports(int side, const hand_rigid_contact::Plane* planes, const double* age_seconds, int count) {
    if(side<0 || side>1) return;
    Side& s=g_side[side];
    if(count==0) {s.support_count=0;return;}
    if(count<0 || count>80 || !planes || !age_seconds || !s.verified || !s.motion.valid) return;
    s.support_count=0;
    const double now=perf::NowMs(); const FQuat inverse=s.motion.rotation.Inverse();
    for(int i=0;i<count;++i) {
        const auto& p=planes[i]; const double age=age_seconds[i];
        if(!std::isfinite(age)||age<0||age>=.05 || !std::isfinite(Length(p.point)) ||
           !std::isfinite(Length(p.sample)) || !std::isfinite(Length(p.normal)) || Length(p.normal)<.5 ||
           !std::isfinite(p.radius)||p.radius<0||p.radius>20) continue;
        auto& saved=s.supports[s.support_count++];
        saved.plane={p.point,Normalize(p.normal),inverse.Rotate(p.sample-s.motion.position),p.radius,.05-age};
        saved.expires_ms=now+(.05-age)*1000.0;
    }
}
void SetPalmShape(int side, const hand_motion::PalmShape& shape) {
    if (side < 0 || side > 1) return;
    const auto& before = g_palm[side];
    if (before.valid != shape.valid || Length(before.centre - shape.centre) > 0.5 ||
        Length(before.half - shape.half) > 0.25 ||
        AngleBetween(before.rotation.Rotate({0,0,1}), shape.rotation.Rotate({0,0,1})) > 5.0*kDegToRad)
        Reset(side);
    g_palm[side] = shape;
}
bool Enabled() { return S.on; }
hand_motion::Result Motion(int side) { return side >= 0 && side < 2 ? g_side[side].result : hand_motion::Result{}; }
double Radius() { return S.motion.radius_cm; }
bool Touching(int side) {
    return side >= 0 && side < 2 && S.on && (g_side[side].result.touching || g_side[side].ahead);
}
bool Contact(int side, FVector* point, FVector* normal) {
    if (side < 0 || side > 1 || !S.on || !point || !normal) return false;
    Side& s = g_side[side];
    if (!s.have_plane || perf::NowMs() - s.plane_ms > 100.0 ||
        (s.plane_actor.obj && !Alive(s.plane_actor)) ||
        Length(s.result.position - s.plane_point) > 20.0 ||
        Dot(s.result.position - s.plane_point, s.plane_normal) < 0.0) {
        s.have_plane = false;
        return false;
    }
    *point = s.plane_point;
    *normal = s.plane_normal;
    return true;
}
} // namespace holevr::vr::body::hand_walls
