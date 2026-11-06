#include "vr/body/leg_world.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include "ue/call.h"

namespace holevr::vr::body::leg_world {
TerrainOffsets ReadTerrainOffsets(ue::UObject mesh) {
    TerrainOffsets out;
    if (!ue::IsLive(mesh)) return out;
    ue::Call get(mesh, "GetAnimInstance");
    if (!get.Valid() || !get.Invoke()) return out;
    const ue::UObject anim = get.Result<ue::UObject>();
    if (!ue::IsLive(anim)) return out;
    out.known = ue::GetProp(anim, "FootOffset_L_Location", out.foot_location[0]) &&
                ue::GetProp(anim, "FootOffset_R_Location", out.foot_location[1]) &&
                ue::GetProp(anim, "PelvisOffset", out.pelvis_location) &&
                ue::GetProp(anim, "PelvisAlpha", out.pelvis_alpha) &&
                ue::GetProp(anim, "IK_TraceDistanceAboveFoot", out.trace_above);
    return out;
}

MovementState ReadState(ue::UObject player) {
    MovementState out;
    if (!ue::IsLive(player)) return out;
    ue::UObject movement = 0, kick = 0;
    if (!ue::GetProp(player, "CharacterMovement", movement) || !ue::IsLive(movement) ||
        !ue::GetProp(player, "TL_Kick", kick) || !ue::IsLive(kick)) return out;
    ue::Call ground(movement, "IsMovingOnGround");
    ue::Call playing(kick, "IsPlaying");
    if (!ground.Valid() || !playing.Valid()) return out;
    int32_t a = 0, b = 0;
    if (!ground.Raw("ReturnValue", &a) || a != sizeof(bool) ||
        !playing.Raw("ReturnValue", &b) || b != sizeof(bool)) return out;
    if (!ground.Invoke() || !playing.Invoke()) return out;
    out.known = true;
    out.grounded = ground.Result<bool>();
    out.kicking = playing.Result<bool>();
    ue::UObject capsule = 0;
    if (ue::GetProp(player, "CapsuleComponent", capsule) && ue::IsLive(capsule)) {
        ue::Call location(capsule, "K2_GetComponentLocation");
        ue::Call height(capsule, "GetScaledCapsuleHalfHeight");
        int32_t location_size = 0, height_size = 0;
        if (location.Valid() && height.Valid() && location.Raw("ReturnValue", &location_size) &&
            location_size == sizeof(FVector) && height.Raw("ReturnValue", &height_size) &&
            height_size == sizeof(float) && location.Invoke() && height.Invoke()) {
            const FVector at = location.Result<FVector>();
            const double half = height.Result<float>();
            if (std::isfinite(at.Z) && std::isfinite(half) && half > 1 && half < 300) {
                out.floor_z = at.Z - half;
                out.floor_known = true;
            }
        }
    }
    return out;
}

namespace {
bool Finite(FVector p) { return std::isfinite(p.X)&&std::isfinite(p.Y)&&std::isfinite(p.Z); }
legs::Ground Trace(ue::UObject player,const FVector& start,const FVector& end,
                   const std::vector<ue::UObject>& ignored,bool need_frame,bool complex) {
    legs::Ground out;
    if(!ue::IsLive(player)||!Finite(start)||!Finite(end)) return out;
    static ue::ObjectRef ksl("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
    static ue::ObjectRef gps("GameplayStatics /Script/Engine.Default__GameplayStatics");
    const ue::UObject trace_library=ksl.Get(),hit_library=gps.Get();
    if(!trace_library||!hit_library) return out;
    struct Array { const ue::UObject* data; int32_t num,max; };
    const Array array{ignored.data(),static_cast<int32_t>(ignored.size()),static_cast<int32_t>(ignored.size())};
    ue::Call trace(trace_library,"LineTraceSingle");
    if(!trace.Valid()) return out;
    trace.Set("WorldContextObject",player).Set("Start",start).Set("End",end)
         .Set("TraceChannel",uint8_t{0}).Set("bTraceComplex",complex)
         .Set("ActorsToIgnore",array).Set("bIgnoreSelf",true);
    if(!trace.Invoke()||!trace.Result<bool>()) return out;
    int32_t hit_size=0,in_size=0;
    const uint8_t* hit=trace.Raw("OutHit",&hit_size);
    ue::Call split(hit_library,"BreakHitResult");
    uint8_t* in=split.Raw("Hit",&in_size);
    if(!hit||!in||hit_size!=in_size||hit_size<=0) return out;
    std::memcpy(in,hit,static_cast<size_t>(hit_size));
    if(!split.Invoke()||!split.Get<bool>("bBlockingHit")||split.Get<bool>("bInitialOverlap")) return out;
    out.hit=true; out.point=split.Get<FVector>("ImpactPoint"); out.normal=split.Get<FVector>("ImpactNormal");
    const ue::UObject component=split.Get<ue::UObject>("HitComponent");
    if(!ue::IsLive(component)) return {};
    out.support_id=component;
    if(need_frame) {
        ue::Call transform(component,"K2_GetComponentToWorld"); int32_t size=0;
        if(!transform.Valid()||!transform.Raw("ReturnValue",&size)||size!=sizeof(skel::Xf)||!transform.Invoke()) return {};
        out.support_frame=transform.Result<skel::Xf>();
    }
    return out;
}
std::vector<ue::UObject> Ignored(ue::UObject player,const std::vector<ue::UObject>& extra) {
    std::vector<ue::UObject> out; out.reserve(extra.size()+1); out.push_back(player);
    for(auto actor:extra) if(actor!=player&&ue::IsLive(actor)) out.push_back(actor);
    return out;
}
}
void ProbeFeet(ProbeState& state,ue::UObject player,const legs::Input& input,
               legs::Ground (&ground)[2],const std::vector<ue::UObject>& extra_ignore) {
    ground[0]={}; ground[1]={}; state.diagnostics.traces_last=0;
    const bool reset=!ue::IsLive(player)||!input.state_known||!input.floor_known||!input.grounded||input.kicking||
        !Finite(input.origin)||!std::isfinite(input.floor_z)||!std::isfinite(input.dt)||input.dt<=0||input.dt>.15||
        !std::isfinite(input.speed)||input.speed<0||
        (state.player&&state.player!=player)||
        (state.have_origin&&ik::Length(input.origin-state.last_origin)>100);
    if(reset) { const auto diagnostics=state.diagnostics; state={}; state.diagnostics=diagnostics; return; }
    state.have_origin=true; state.last_origin=input.origin; state.player=player;
    state.profile_age+=input.dt;
    state.retry_seconds=std::max(0.0,state.retry_seconds-input.dt);
    if(state.profile_age>.75) state.profile={};
    const auto ignored=Ignored(player,extra_ignore);
    auto trace=[&](FVector from,FVector to,bool frame,bool complex=true) {
        if(state.diagnostics.traces_last>=9) return legs::Ground{};
        ++state.diagnostics.traces_last; ++state.diagnostics.traces_total;
        return Trace(player,from,to,ignored,frame,complex);
    };
    FVector velocity=Finite(input.velocity)?input.velocity:FVector{};
    velocity.Z=0;
    FVector direction=state.profile.valid?state.profile.direction:
        (ik::Length(velocity)>12?ik::Normalize(velocity):ik::Normalize(FVector{input.forward.X,input.forward.Y,0},{1,0,0}));
    if(!state.scanning&&((!state.profile.valid&&state.retry_seconds<=0)||
                        (state.profile.valid&&state.profile_age>.12&&state.retry_seconds<=0))) {
        state.scanning=true; state.scan_age=0; state.next_sample=0;
        const FVector lead=velocity*std::min(.15,20.0/std::max(1.0,ik::Length(velocity)));
        state.scan_origin={input.origin.X+lead.X,input.origin.Y+lead.Y,input.floor_z};
        state.scan_direction=direction; state.scan_floor=input.floor_z;
    }
    if(state.scanning) {
        state.scan_age+=input.dt;
        if(state.scan_age>.8) {
            state.scanning=false; ++state.diagnostics.scans_rejected;
            state.retry_seconds=input.speed>=24?.15:.3;
        }
        else for(unsigned budget=0;budget<3&&state.next_sample<stairs::kSamples;++budget) {
            const double along=(int(state.next_sample)-stairs::kSamples/2)*stairs::kSpacing;
            const FVector at=state.scan_origin+state.scan_direction*along;
            const auto g=trace(at+FVector{0,0,90},at-FVector{0,0,90},false);
            auto& sample=state.samples[state.next_sample++];
            sample={g.hit,along,g.point.Z,g.normal,g.support_id};
            if(state.next_sample==stairs::kSamples) {
                const auto found=stairs::Recognize(state.samples,state.scan_origin,state.scan_direction,state.scan_floor);
                if(found.valid) { state.profile=found; state.profile_age=0; ++state.diagnostics.scans_accepted; }
                else { ++state.diagnostics.scans_rejected; state.retry_seconds=input.speed>=24?.15:.3; }
                state.scanning=false;
            }
        }
    }
    for(int side=0;side<2;++side) {
        if(!input.foot[side].valid) continue;
        const FVector sole=legs::ProbeSole(input,side);
        const double along_speed=velocity.X*direction.X+velocity.Y*direction.Y;
        const auto landing=stairs::Select(state.profile,sole,input.floor_z,along_speed*.12);
        unsigned used=0;
        auto foot_trace=[&](FVector at,double above,double below,bool frame,bool complex=true) {
            ++used; return trace(at+FVector{0,0,above},at-FVector{0,0,below},frame,complex);
        };
        if(landing.valid) {
            const auto centre=foot_trace(landing.sole,6,8,true);
            auto agrees=[&](const legs::Ground& g,FVector expected) {
                const double normal=ik::Length(g.normal);
                return g.hit&&!g.initial_overlap&&g.support_id==landing.support_id&&Finite(g.point)&&Finite(g.normal)&&
                    normal>.5&&normal<1.5&&g.normal.Z/normal>=.94&&
                    std::fabs(g.point.Z-landing.sole.Z)<=2&&std::hypot(g.point.X-expected.X,g.point.Y-expected.Y)<=1;
            };
            if(agrees(centre,landing.sole)) {
                const FVector heel=landing.sole-state.profile.direction*6.5;
                const FVector toe=landing.sole+state.profile.direction*6.5;
                if(agrees(foot_trace(heel,6,8,false),heel)&&agrees(foot_trace(toe,6,8,false),toe)) {
                    auto qualified=centre; qualified.stair=true; qualified.clearance_z=landing.clearance_z;
                    if(legs::ValidGround(qualified,sole)) { ground[side]=qualified; ++state.diagnostics.stair_feet; continue; }
                }
            }
        }
        // Failed three-point qualification cannot consume a fourth foot trace.
        if(used<3) {
            const auto ordinary=foot_trace(sole,12,18,true,false);
            if(legs::ValidGround(ordinary,sole)) ground[side]=ordinary;
        }
    }
    state.diagnostics.traces_peak=std::max(state.diagnostics.traces_peak,state.diagnostics.traces_last);
}
} // namespace holevr::vr::body::leg_world
