#include "vr/body/wrist_calibration.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "vr/body/ik_math.h"

namespace holevr::vr::wrist_calibration {
namespace {
using namespace ik;
constexpr double kForearmRms = 8, kPalmRms = 10, kMaximumError = 20;
constexpr double kAngleEpsilon = 1e-6; // inclusive angular gates survive acos/normalization roundoff
bool Finite(FVector v) { return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z); }
bool UnitQuat(const FQuat& q,FQuat* out) {
    const double n=q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
    if(!std::isfinite(n)||n<1e-12) return false;
    const double s=1/std::sqrt(n);
    *out={q.X*s,q.Y*s,q.Z*s,q.W*s};
    return true;
}
bool Direction(FVector v) { const double n=Length(v); return Finite(v)&&std::isfinite(n)&&n>.1; }
bool Offset(FVector v) { return Finite(v)&&Length(v)<=50; }
double Degrees(FVector a,FVector b) {
    return std::acos(std::clamp(Dot(a,b),-1.0,1.0))*kRadToDeg;
}
struct Pair { FVector forearm,palm; };
struct Group { std::vector<Pair> pairs; FVector forearm,palm; unsigned observed=0; };
bool EnoughInliers(std::size_t accepted,std::size_t observed) {
    return accepted>=3 && accepted*20>=observed*17; // at least 85% of each separate hold
}
Reason RobustGroup(Group& g) {
    if(g.observed<3) return Reason::NotEnoughSamples;
    if(!EnoughInliers(g.pairs.size(),g.observed)) return Reason::InconsistentPalm;
    FVector forearm,palm;
    for(const auto& pair:g.pairs) { forearm=forearm+pair.forearm; palm=palm+pair.palm; }
    if(Length(forearm)<.1) return Reason::InconsistentForearm;
    if(Length(palm)<.1) return Reason::InconsistentPalm;
    forearm=Normalize(forearm); palm=Normalize(palm);
    // A robust centre in the joint pair of unit-vector spaces. Inverse chord
    // distance weighting limits one orientation spike without choosing axes
    // from coordinate-wise medians. The distance/rotation operations are
    // invariant to the arbitrary controller frame, and work is O(samples).
    for(int iteration=0;iteration<16;++iteration) {
        FVector f,p;
        for(const auto& pair:g.pairs) {
            const FVector df=pair.forearm-forearm,dp=pair.palm-palm;
            const double weight=1/std::max(.01,std::sqrt(Dot(df,df)+Dot(dp,dp)));
            f=f+pair.forearm*weight; p=p+pair.palm*weight;
        }
        if(Length(f)<.1) return Reason::InconsistentForearm;
        if(Length(p)<.1) return Reason::InconsistentPalm;
        forearm=Normalize(f); palm=Normalize(p);
    }
    std::vector<Pair> accepted;
    bool rejected_forearm=false;
    for(const auto& pair:g.pairs) {
        const double fe=Degrees(forearm,pair.forearm),pe=Degrees(palm,pair.palm);
        if(fe<=kMaximumError+kAngleEpsilon && pe<=kMaximumError+kAngleEpsilon)
            accepted.push_back(pair);
        else rejected_forearm |= fe>kMaximumError+kAngleEpsilon;
    }
    if(!EnoughInliers(accepted.size(),g.observed))
        return rejected_forearm?Reason::InconsistentForearm:Reason::InconsistentPalm;
    g.pairs=std::move(accepted); g.forearm={}; g.palm={};
    for(const auto& pair:g.pairs) { g.forearm=g.forearm+pair.forearm; g.palm=g.palm+pair.palm; }
    g.forearm=g.forearm*(1.0/g.pairs.size()); g.palm=g.palm*(1.0/g.pairs.size());
    if(Length(g.forearm)<.9) return Reason::InconsistentForearm;
    if(Length(g.palm)<.9) return Reason::InconsistentPalm;
    return Reason::None;
}
}

bool Validate(const Calibration& c) {
    return c.measured && Offset(c.wrist_offset_cm) && Finite(c.forearm_axis_grip) && Finite(c.palm_axis_grip) &&
        std::fabs(Length(c.forearm_axis_grip)-1)<1e-4 && std::fabs(Length(c.palm_axis_grip)-1)<1e-4 &&
        std::fabs(Dot(c.forearm_axis_grip,c.palm_axis_grip))<1e-4 &&
        std::isfinite(c.forearm_rms_deg) && c.forearm_rms_deg>=0 && c.forearm_rms_deg<=kForearmRms+kAngleEpsilon &&
        std::isfinite(c.palm_rms_deg) && c.palm_rms_deg>=0 && c.palm_rms_deg<=kPalmRms+kAngleEpsilon &&
        std::isfinite(c.max_error_deg) && c.max_error_deg>=0 && c.max_error_deg<=kMaximumError+kAngleEpsilon;
}

Result Fit(const std::vector<Observation>& observations,const FVector& frozen_wrist_offset_cm) {
    Result result;
    auto fail=[&](Reason reason) { result={}; result.reason=reason; return result; };
    if(!Offset(frozen_wrist_offset_cm)) return fail(Reason::InvalidOffset);
    if(observations.size()>4096) return fail(Reason::InvalidSample);
    Group group[2];
    for(const auto& sample:observations) {
        if(sample.stage==Stage::Overhead) continue;
        if(sample.stage!=Stage::Forward && sample.stage!=Stage::Out) return fail(Reason::InvalidSample);
        FQuat raw;
        if(!sample.palm_known || !UnitQuat(sample.raw_grip_rotation,&raw) ||
            !Direction(sample.forearm_direction) || !Direction(sample.palm_direction)) return fail(Reason::InvalidSample);
        Group& g=group[sample.stage==Stage::Forward?0:1];
        ++g.observed;
        const FVector forearm=Normalize(sample.forearm_direction),palm=Normalize(sample.palm_direction);
        // A stated neutral palm almost parallel to the inferred forearm is
        // contradictory; do not silently rotate that one pose into compliance.
        if(std::fabs(Dot(forearm,palm))>std::sin((kMaximumError+kAngleEpsilon)*kDegToRad)) continue;
        // "Palm down/inward" specifies roll around the forearm. A slightly
        // tilted but accepted straight arm cannot also have a perpendicular
        // palm exactly on that world axis. Use its projection as the roll
        // reference, after the nonorthogonality gate above. This same rule
        // defines one constant anatomical basis for both holds.
        const FVector palm_roll=Normalize(Reject(palm,forearm));
        Pair pair{raw.Inverse().Rotate(forearm),raw.Inverse().Rotate(palm_roll)};
        g.pairs.push_back(pair);
    }
    for(auto& g:group) {
        const auto reason=RobustGroup(g);
        if(reason!=Reason::None) return fail(reason);
    }
    Calibration c;
    const FVector f=group[0].forearm+group[1].forearm;
    if(Length(f)<1) return fail(Reason::InconsistentForearm);
    c.forearm_axis_grip=Normalize(f);
    const FVector p=Reject(group[0].palm+group[1].palm,c.forearm_axis_grip);
    if(Length(p)<1) return fail(Reason::InconsistentPalm);
    c.palm_axis_grip=Normalize(p);
    double forearm_square=0,palm_square=0;
    for(const auto& g:group) {
        double f_square=0,p_square=0,max_f=0,max_p=0;
        for(const auto& pair:g.pairs) {
            const double fe=Degrees(c.forearm_axis_grip,pair.forearm),pe=Degrees(c.palm_axis_grip,pair.palm);
            f_square+=fe*fe; p_square+=pe*pe;
            max_f=std::max(max_f,fe); max_p=std::max(max_p,pe);
        }
        f_square/=g.pairs.size(); p_square/=g.pairs.size();
        // Gate each stage's accepted window separately; at least 85% of that
        // stage survived its own checks. Equal stage weight still prevents
        // a dense good stage from hiding a coherent wrong pose in the other.
        if(std::sqrt(f_square)>kForearmRms+kAngleEpsilon || max_f>kMaximumError+kAngleEpsilon) return fail(Reason::InconsistentForearm);
        if(std::sqrt(p_square)>kPalmRms+kAngleEpsilon || max_p>kMaximumError+kAngleEpsilon) return fail(Reason::InconsistentPalm);
        forearm_square+=f_square*.5; palm_square+=p_square*.5;
        c.max_error_deg=std::max(c.max_error_deg,std::max(max_f,max_p));
    }
    c.measured=true;
    c.wrist_offset_cm=frozen_wrist_offset_cm;
    c.forearm_rms_deg=std::sqrt(forearm_square); c.palm_rms_deg=std::sqrt(palm_square);
    result.calibration=c;
    return result;
}

bool Map(const Calibration& c,const Mapping& in,FVector* wrist_world,FQuat* hand_world) {
    if(!wrist_world || !hand_world || !Validate(c) || !Finite(in.controller_world) || !Offset(in.position_delta_cm) ||
        !std::isfinite(in.tracking_scale) || in.tracking_scale<=0 || in.tracking_scale>100 ||
        !Direction(in.forearm_axis_hand) || !Direction(in.palm_axis_hand)) return false;
    FQuat grip,manual;
    if(!UnitQuat(in.grip_world,&grip) || !UnitQuat(in.rotation_delta_grip,&manual)) return false;
    const FVector source_forearm=Normalize(in.forearm_axis_hand);
    const FVector source_palm=Reject(in.palm_axis_hand,source_forearm);
    if(Length(source_palm)<.1) return false;
    const FQuat corrected=grip*manual;
    const FVector forearm=corrected.Rotate(c.forearm_axis_grip),palm=corrected.Rotate(c.palm_axis_grip);
    const FQuat rotation=AlignFrames(source_forearm,Normalize(source_palm),forearm,palm);
    const FVector position=in.controller_world+grip.Rotate((c.wrist_offset_cm+in.position_delta_cm)*in.tracking_scale);
    FQuat normalized;
    if(!Finite(position) || !UnitQuat(rotation,&normalized)) return false;
    *wrist_world=position; *hand_world=normalized;
    return true;
}

} // namespace holevr::vr::wrist_calibration
