#include "vr/body/finger_world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "ue/call.h"
#include "vr/body/hand_walls.h"
#include "vr/wall_shapes.h"

namespace holevr::vr::finger_world {
namespace {
using namespace finger_world_query;
bool g_enabled = true;
wall_shapes::Mode g_shapes = wall_shapes::Mode::kNearest;
Settings g_settings;
State g_state[2];
ue::UObject g_player[2]{};
ue::UObject g_ksl=0, g_gps=0;
double g_next_lookup_ms=0;
perf::Counter g_perf{"body individual finger geometry queries", {}, {}, {}, true};
perf::Counter g_verify_perf{"body final finger geometry verification", {}, {}, {}, true};

struct Adapter final : Backend {
    ue::Call trace, hit_break;
    int radius_size=0, trace_hit_size=0, break_hit_size=0;
    bool usable=false;
    int invocations=0;
    const std::vector<ue::UObject>& ignore;

    Adapter(ue::UObject player, const std::vector<ue::UObject>& ignored)
        : trace(g_ksl,"SphereTraceSingle"), hit_break(g_gps,"BreakHitResult"), ignore(ignored) {
        if (!trace.Valid() || !hit_break.Valid()) return;
        // Call::Set/Get deliberately tolerate missing/mismatched parameters.
        // Validate the complete contract first so reflection changes cannot
        // silently become a clear trace or a zero-valued impact position.
        auto sized=[](ue::Call& call,const char* name,int expected) {
            int actual=0;
            return call.Raw(name,&actual) && actual==expected;
        };
        struct Arr { const ue::UObject* data; int32_t num,max; };
        if (!sized(trace,"WorldContextObject",sizeof(ue::UObject)) ||
            !sized(trace,"Start",sizeof(FVector)) || !sized(trace,"End",sizeof(FVector)) ||
            !sized(trace,"TraceChannel",sizeof(uint8_t)) || !sized(trace,"bTraceComplex",sizeof(bool)) ||
            !sized(trace,"ActorsToIgnore",sizeof(Arr)) || !sized(trace,"bIgnoreSelf",sizeof(bool)) ||
            !sized(trace,"ReturnValue",sizeof(bool)) ||
            !sized(hit_break,"bBlockingHit",sizeof(bool)) || !sized(hit_break,"bInitialOverlap",sizeof(bool)) ||
            !sized(hit_break,"Location",sizeof(FVector)) || !sized(hit_break,"ImpactPoint",sizeof(FVector)) ||
            !sized(hit_break,"ImpactNormal",sizeof(FVector)) || !sized(hit_break,"Normal",sizeof(FVector)) ||
            !sized(hit_break,"HitActor",sizeof(ue::UObject)) ||
            !sized(hit_break,"HitComponent",sizeof(ue::UObject))) return;
        trace.Raw("Radius",&radius_size);
        trace.Raw("OutHit",&trace_hit_size);
        hit_break.Raw("Hit",&break_hit_size);
        if ((radius_size != 4 && radius_size != 8) || trace_hit_size <= 0 || trace_hit_size != break_hit_size) return;
        const Arr array{ignore.data(),static_cast<int32_t>(ignore.size()),static_cast<int32_t>(ignore.size())};
        trace.Set("WorldContextObject",player).Set("TraceChannel",uint8_t{0}).Set("bTraceComplex",false)
             .Set("ActorsToIgnore",array).Set("bIgnoreSelf",true);
        usable=true;
    }

    // The sweep against the shapes the wall_shape setting names; with both,
    // the nearest hit wins (an overlap at the start first). Either failing
    // fails the whole sweep.
    QueryStatus Sweep(const FVector& from, const FVector& to, double radius_cm, Hit& hit) override {
        hit={};
        if (!usable) return QueryStatus::unavailable;
        trace.Set("Start",from).Set("End",to);
        if (radius_size==4) trace.Set("Radius",static_cast<float>(radius_cm));
        else trace.Set("Radius",radius_cm);
        QueryStatus best=QueryStatus::clear;
        const int passes=wall_shapes::Passes(g_shapes);
        for (int pass=0; pass<passes; ++pass) {
            trace.Set("bTraceComplex",wall_shapes::Complex(g_shapes,pass));
            Hit got;
            const QueryStatus status=One(got);
            if (status==QueryStatus::unavailable) { hit={}; return status; }
            if (status!=QueryStatus::hit) continue;
            const auto distance=[&](const Hit& h) {
                const FVector d=h.center-from;
                return d.X*d.X+d.Y*d.Y+d.Z*d.Z;
            };
            if (best!=QueryStatus::hit ||
                (!hit.initial_overlap && (got.initial_overlap || distance(got)<distance(hit)))) {
                hit=got;
                best=QueryStatus::hit;
            }
        }
        return best;
    }

    // One sweep as the call is set up now.
    QueryStatus One(Hit& hit) {
        hit={};
        ++invocations;
        if (!trace.Invoke()) return QueryStatus::unavailable;
        if (!trace.Result<bool>()) return QueryStatus::clear;
        int32_t source_size=0,target_size=0;
        const auto* source=trace.Raw("OutHit",&source_size);
        auto* target=hit_break.Raw("Hit",&target_size);
        if (!source || !target || source_size!=trace_hit_size || target_size!=source_size)
            return QueryStatus::unavailable;
        std::memcpy(target,source,static_cast<size_t>(source_size));
        ++invocations;
        if (!hit_break.Invoke()) return QueryStatus::unavailable;
        if (!hit_break.Get<bool>("bBlockingHit")) return QueryStatus::clear;
        const auto actor=hit_break.Get<ue::UObject>("HitActor");
        // Engine-side ignores are authoritative. This additional check makes
        // unexpected ignored/dead actors a failed observation, never a plane.
        if (actor && (!ue::IsLive(actor) || std::find(ignore.begin(),ignore.end(),actor)!=ignore.end()))
            return QueryStatus::unavailable;
        hit.center=hit_break.Get<FVector>("Location");
        hit.point=hit_break.Get<FVector>("ImpactPoint");
        hit.initial_overlap=hit_break.Get<bool>("bInitialOverlap");
        hit.normal=hit_break.Get<FVector>(hit.initial_overlap ? "Normal" : "ImpactNormal");
        const auto component=hit_break.Get<ue::UObject>("HitComponent");
        if(component && !ue::IsLive(component)) return QueryStatus::unavailable;
        hit.surface_id=component ? component : actor;
        return QueryStatus::hit;
    }
    bool SurfaceTransform(std::uint64_t identity,skel::Xf& frame) override {
        const auto component=static_cast<ue::UObject>(identity);
        // Only a rigid, ordinary static mesh has one transform for all its
        // collision features. Instanced/skeletal/procedural subclasses and
        // actor fallbacks intentionally use real sweeps instead.
        if(!ue::IsLive(component) || ue::GetName(ue::GetClass(component))!="StaticMeshComponent") return false;
        const auto owner=ue::GetOuter(component);
        if(std::find(ignore.begin(),ignore.end(),owner)!=ignore.end()) return false;
        ue::Call transform(component,"K2_GetComponentToWorld");
        int32_t size=0;
        if(!transform.Valid() || !transform.Raw("ReturnValue",&size) || size!=sizeof(skel::Xf)) return false;
        ++invocations;
        if(!transform.Invoke()) return false;
        frame=transform.Result<skel::Xf>();
        return true;
    }
    int InvocationCount() const override { return invocations; }
};
}  // namespace

void Reset(int side) {
    if (side<0) {
        for (int i=0;i<2;++i) { finger_world_query::Reset(g_state[i]); g_player[i]=0; }
    } else if (side<2) { finger_world_query::Reset(g_state[side]); g_player[side]=0; }
}
void LoadSettings() {
    g_enabled=config::GetBool("hand_individual_finger_collision",true);
    g_shapes=wall_shapes::ModeSetting();  // the shapes you see and the hit boxes, nearest first
    g_settings.max_casts=std::clamp(config::GetInt("hand_finger_query_budget",32),16,64);
    const double refresh=config::GetFloat("hand_finger_contact_refresh_ms",50.f)/1000.0;
    g_settings.clear_refresh_seconds=std::clamp(std::isfinite(refresh)?refresh:0.05,0.005,0.05);
    g_settings.max_contact_age_seconds=g_settings.clear_refresh_seconds;
    Reset();
}
bool Enabled() { return g_enabled; }
void InvalidateGeometry(int side) {
    if(side>=0 && side<2) finger_world_query::InvalidateGeometry(g_state[side]);
}
Verification VerifyGeometry(ue::UObject player,int side,const fingers::Chain chains[5],
                            const fingers::Angles resolved[5],const skel::Xf& hand,double radius_cm) {
    if(side<0 || side>1 || !g_enabled || !player || player!=g_player[side] ||
       !ue::IsLive(player) || !g_ksl || !g_gps) return {};
    perf::Scope timer(g_verify_perf);
    Adapter backend(player,body::hand_walls::IgnoredActors());
    if(!backend.usable) return {};
    return finger_world_query::VerifyGeometry(g_state[side],MakeGeometry(chains,resolved,hand,radius_cm),
                                              backend,g_settings);
}

Stats Update(ue::UObject player, int side, const fingers::Chain chains[5],
             const fingers::Angles proposed[5], const skel::Xf& hand,
             double radius_cm, double dt, finger_contact::Contacts output[5]) {
    for (int f=0;f<5;++f) output[f]={};
    if (side<0 || side>1) return {};
    if (!g_enabled || !player || !ue::IsLive(player)) { Reset(side); return {}; }
    const bool changed_player = g_player[side] != player;
    if (changed_player) { Reset(side); g_player[side]=player; }
    const double now=perf::NowMs();
    if ((!g_ksl || !g_gps) && now>=g_next_lookup_ms) {
        g_next_lookup_ms=now+5000;
        g_ksl=ue::FindObject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        g_gps=ue::FindObject("GameplayStatics /Script/Engine.Default__GameplayStatics");
    }
    if (!g_ksl || !g_gps) { Reset(side); return {}; }
    static const bool registered=[] { perf::Register(&g_perf); perf::Register(&g_verify_perf); return true; }();
    (void)registered;
    perf::Scope timer(g_perf);
    Adapter backend(player,body::hand_walls::IgnoredActors());
    if (!backend.usable) { Reset(side); return {}; }
    auto stats=finger_world_query::Update(g_state[side],MakeGeometry(chains,proposed,hand,radius_cm),
                                          dt,backend,output,g_settings);
    stats.reset=stats.reset || changed_player;
    return stats;
}
void Commit(int side, const fingers::Chain chains[5], const fingers::Angles resolved[5],
            const skel::Xf& hand, double radius_cm) {
    if (side<0 || side>1 || !g_enabled) return;
    finger_world_query::Commit(g_state[side],MakeGeometry(chains,resolved,hand,radius_cm));
}
}  // namespace holevr::vr::finger_world
