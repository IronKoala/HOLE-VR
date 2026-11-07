#include "vr/body/arm_pose.h"

#include <algorithm>
#include <limits>

namespace holevr::vr::arm_pose {
namespace {
using namespace ik;

bool Finite(const FVector& v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}
bool ValidRotation(const FQuat& q) {
    const double n = q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W;
    return std::isfinite(n) && n > 1e-12;
}

// Used only if the supplied anatomical fallback is also parallel. Choose
// the least-aligned basis axis, with a fixed tie order, to avoid zero/NaN.
FVector Perpendicular(const FVector& axis, const FVector& preferred) {
    FVector bend = Finite(preferred) ? Reject(preferred, axis) : FVector{};
    if (Length(bend) > 1e-6) return Normalize(bend);
    const FVector basis[] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(Dot(axis, basis[i])) < std::fabs(Dot(axis, basis[best]))) best = i;
    return Normalize(Reject(basis[best], axis));
}
}  // namespace

Reference MeasureReference(const skel::Xf& upper, const skel::Xf& forearm,
                           const skel::Xf& hand, const FVector& preferred_bend) {
    Reference r;
    if (!Finite(upper.translation) || !Finite(forearm.translation) || !Finite(hand.translation) ||
        !ValidRotation(upper.rotation) || !ValidRotation(forearm.rotation) || !ValidRotation(hand.rotation))
        return r;
    const FVector a = forearm.translation - upper.translation;
    const FVector b = hand.translation - forearm.translation;
    r.upper_length = Length(a);
    r.forearm_length = Length(b);
    if (r.upper_length < 1e-4 || r.forearm_length < 1e-4) return r;

    const FVector normal_raw = Cross(a, b);
    const FVector direction = Normalize(hand.translation - upper.translation, Normalize(a));
    // Very slightly bent reference poses need the same deterministic plane
    // as a straight T/A pose; tiny export roundoff must not choose arm roll.
    const FVector normal = Length(normal_raw) > 1e-3 * r.upper_length * r.forearm_length
        ? Normalize(normal_raw) : Normalize(Cross(Perpendicular(direction, preferred_bend), direction));
    const FQuat uq = Normalized(upper.rotation), fq = Normalized(forearm.rotation);
    r.upper_axis = uq.Inverse().Rotate(Normalize(a));
    r.forearm_axis = fq.Inverse().Rotate(Normalize(b));
    r.upper_normal = uq.Inverse().Rotate(normal);
    r.forearm_normal = fq.Inverse().Rotate(normal);
    r.forearm_to_hand = Normalized(fq.Inverse() * Normalized(hand.rotation));
    r.upper_scale = upper.scale;
    r.forearm_scale = forearm.scale;
    r.valid = true;
    return r;
}

Rotations Orient(const Reference& r, const FVector& shoulder,
                 const ik::TwoBoneResult& solved, const FVector& pole) {
    Rotations out;
    if (!r.valid || !Finite(shoulder) || !Finite(solved.mid) || !Finite(solved.end)) return out;
    const FVector a = solved.mid - shoulder, b = solved.end - solved.mid;
    const double lengths = Length(a) * Length(b);
    if (lengths < 1e-8) return out;
    const FVector normal_raw = Cross(a, b);
    const FVector direction = Normalize(solved.end - shoulder, Normalize(a));
    // A sub-milliradian bend does not provide a reliable plane. Use the
    // supplied continuity pole, as with a straight reference pose, instead
    // of amplifying sign changes in tiny position residuals into bone roll.
    const FVector normal = Length(normal_raw) > 1e-3 * lengths
        ? Normalize(normal_raw) : Normalize(Cross(Perpendicular(direction, pole), direction));
    out.upper = AlignFrames(r.upper_axis, r.upper_normal, a, normal);
    out.forearm = AlignFrames(r.forearm_axis, r.forearm_normal, b, normal);
    out.valid = ValidRotation(out.upper) && ValidRotation(out.forearm);
    return out;
}

bool MatchSegmentScale(const FVector& axis, const FVector& reference_scale,
                       double reference_length, double solved_length,
                       const FVector& baseline, FVector* out) {
    if (!out || !Finite(axis) || !Finite(reference_scale) || !Finite(baseline) ||
        reference_scale.X <= 0 || reference_scale.Y <= 0 || reference_scale.Z <= 0 ||
        baseline.X <= 0 || baseline.Y <= 0 || baseline.Z <= 0 ||
        !std::isfinite(reference_length) || !std::isfinite(solved_length) ||
        reference_length < 1e-4 || solved_length < 1e-4 || Length(axis) < .1) return false;
    const FVector direction = Normalize(axis);
    const double a[] = {std::fabs(direction.X), std::fabs(direction.Y), std::fabs(direction.Z)};
    int longitudinal = 0;
    for (int i = 1; i < 3; ++i) if (a[i] > a[longitudinal]) longitudinal = i;
    // Arbitrarily oriented segment axes would need a shear-capable transform.
    // Do not silently squash an unrelated local axis on an unsupported rig.
    if (a[longitudinal] < 1.0 - 1e-6) return false;
    const double reference[] = {reference_scale.X, reference_scale.Y, reference_scale.Z};
    const double value = reference[longitudinal] * solved_length / reference_length;
    if (!std::isfinite(value) || value < 1e-6 || value > 1000) return false;
    FVector result = baseline;
    if (longitudinal == 0) result.X = value;
    else if (longitudinal == 1) result.Y = value;
    else result.Z = value;
    *out = result;
    return true;
}

double WristTwist(const Reference& r, const FQuat& forearm_world, const FQuat& hand_world) {
    if (!r.valid || !ValidRotation(forearm_world) || !ValidRotation(hand_world)) return 0;
    const FQuat relative = Normalized(Normalized(forearm_world).Inverse() * Normalized(hand_world));
    return TwistAngle(Normalized(relative * r.forearm_to_hand.Inverse()), r.forearm_axis);
}

WristPole WristAlignedPole(const Reference& r, const FVector& shoulder,
                           const FVector& wrist, const FQuat& hand_world) {
    WristPole out;
    if (!r.valid || !Finite(shoulder) || !Finite(wrist) || !ValidRotation(hand_world) ||
        !Finite(r.forearm_axis) || !ValidRotation(r.forearm_to_hand)) return out;
    const FVector to=wrist-shoulder;
    if (Length(to)<1e-5) return out;
    const FVector wanted=Normalized(hand_world).Rotate(r.forearm_to_hand.Inverse().Rotate(r.forearm_axis));
    const FVector away=Reject(wanted,Normalize(to))*-1.0;
    out.strength=Length(away);
    if (!std::isfinite(out.strength) || out.strength<1e-5) return out;
    out.pole=away*(1.0/out.strength);
    out.valid=true;
    return out;
}

double ForearmRotation(const FVector& upper, const FVector& forearm, const FVector& palm,
                       const FVector& bend_pole, double side_sign) {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    if (!Finite(upper) || !Finite(forearm) || !Finite(palm) || !Finite(bend_pole) ||
        !std::isfinite(side_sign) || Length(forearm) < 1e-6) return nan;
    const FVector f = Normalize(forearm);
    // Bend-plane normal as Orient builds it: cross(upper, forearm); with a
    // straight arm the elbow's pole supplies the plane (pole x axis).
    FVector normal = Cross(upper, forearm);
    if (Length(normal) <= 1e-3 * Length(upper) * Length(forearm)) {
        const FVector axis = upper + forearm;
        normal = Cross(Reject(bend_pole, Normalize(axis, f)), Normalize(axis, f));
    }
    normal = Reject(normal, f);
    if (Length(normal) < 1e-9) return nan;
    normal = Normalize(normal);
    const FVector anterior = Cross(normal, f);          // inside of the bend
    const FVector medial = normal * (side_sign < 0 ? -1.0 : 1.0);
    const FVector p = Reject(palm, f);
    if (Length(p) < .2 * Length(palm) || Length(p) < 1e-9) return nan;
    return std::atan2(Dot(p, medial), Dot(p, anterior));
}

double ReferenceForearmRotation(const Reference& r, const FVector& palm_axis_hand, double side_sign) {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    if (!r.valid || !Finite(palm_axis_hand) || Length(palm_axis_hand) < 1e-6 ||
        !ValidRotation(r.forearm_to_hand) || !Finite(r.forearm_axis) || !Finite(r.forearm_normal) ||
        Length(r.forearm_axis) < .1 || Length(r.forearm_normal) < .1) return nan;
    // Forearm bone space: its axis, the reference bend normal and the palm.
    const FVector f = Normalize(r.forearm_axis);
    const FVector normal = Reject(r.forearm_normal, f);
    if (Length(normal) < .1) return nan;
    const FVector n = Normalize(normal);
    const FVector p = Reject(Normalized(r.forearm_to_hand).Rotate(palm_axis_hand), f);
    if (Length(p) < .2 * Length(palm_axis_hand)) return nan;
    return std::atan2(Dot(p, n * (side_sign < 0 ? -1.0 : 1.0)), Dot(p, Cross(n, f)));
}

SupportTwistResult FollowSupportTwist(const Reference& r, const FQuat& forearm_world,
    const FQuat& hand_world, const FVector& palm_axis_hand, double dt, double share,
    SupportTwistState* state, double initial_forearm_radians) {
    using namespace ik;
    SupportTwistResult out;
    auto invalid = [&] { if(state) *state={}; return out; };
    if(!state || !r.valid || !ValidRotation(forearm_world) || !ValidRotation(hand_world) ||
       !ValidRotation(r.forearm_to_hand) || !Finite(r.forearm_axis) || Length(r.forearm_axis)<.1 ||
       !Finite(palm_axis_hand) || !std::isfinite(dt) || dt<0 || !std::isfinite(share)) return invalid();
    share=std::clamp(share,0.0,1.0);
    const auto forearm=Normalized(forearm_world), hand=Normalized(hand_world);
    const FVector axis=Normalize(forearm.Rotate(r.forearm_axis));
    const FVector hand_axis=Normalize(r.forearm_to_hand.Inverse().Rotate(r.forearm_axis));
    const FVector palm=Reject(palm_axis_hand,hand_axis);
    if(Length(palm)<.1) return invalid();
    const FVector neutral=Reject(forearm.Rotate(r.forearm_to_hand.Rotate(Normalize(palm))),axis);
    const FVector desired=Reject(hand.Rotate(Normalize(palm)),axis);
    if(Length(neutral)<.1) return invalid();
    const auto bounded=[](double a) { return std::isfinite(a)&&std::fabs(a)<=kPi+1e-9; };
    const bool usable=state->valid && bounded(state->source_radians) && bounded(state->target_radians) &&
        bounded(state->forearm_radians) && std::isfinite(state->share) && std::fabs(state->share-share)<1e-9;
    const bool observable=Length(desired)>((usable&&state->observable)?.08:.16);
    if(!usable) {
        *state={};
        if(!observable) return out;
        if(!std::isfinite(initial_forearm_radians)) return invalid();
    }
    const bool initialized=!usable&&observable;
    if(observable) {
        const auto a=Normalize(neutral),b=Normalize(desired);
        const double source=std::atan2(Dot(axis,Cross(a,b)),Dot(a,b));
        if(!usable) {
            state->valid=true; state->share=share;
            state->target_radians=std::remainder(source*share,2*kPi);
            // The fallback already moved this frame's forearm. Starting here
            // prevents a second step or a snap when projection first becomes
            // observable; subsequent frames follow the support target.
            state->forearm_radians=std::remainder(initial_forearm_radians,2*kPi);
        } else if(state->observable && dt>0 && dt<=.25) {
            // Integrate the small physical-frame change, not a principal
            // angle multiplied afresh. Every stored angle stays bounded;
            // no full-turn count can grow during a long support hold.
            const double delta=std::remainder(source-state->source_radians,2*kPi);
            state->target_radians=std::remainder(state->target_radians+delta*share,2*kPi);
        }
        state->source_radians=source;
    }
    state->observable=observable;
    if(dt>0&&!initialized) {
        const double delta=std::remainder(state->target_radians-state->forearm_radians,2*kPi);
        const double step=std::clamp(delta*(1-std::exp(-std::min(dt,.25)/.04)),
            -kTwistBoneRate*std::min(dt,.25),kTwistBoneRate*std::min(dt,.25));
        state->forearm_radians=std::remainder(state->forearm_radians+step,2*kPi);
    }
    out.valid=state->valid; out.observable=observable; out.source_radians=state->source_radians;
    out.target_radians=state->target_radians; out.forearm_radians=state->forearm_radians;
    return out;
}

TwistResult FollowTwist(double target,double dt,double forearm_share,double twist_share,TwistState* state,
                        double center,double watch_share) {
    constexpr double seam_enter=170*kDegToRad, seam_leave=155*kDegToRad, seam_window=.18;
    TwistResult out;
    if (!std::isfinite(target) || !std::isfinite(forearm_share) || !std::isfinite(twist_share) ||
        !std::isfinite(center)) {
        if(state) *state={};
        return out;
    }
    out.valid=true;
    out.raw_radians=std::remainder(target,2*kPi);
    center=std::remainder(center,2*kPi);
    // Roll measured from the center; the seam (and the voting) lives at +/-pi
    // of this, the shares still multiply the whole roll from the reference.
    const double centered=std::remainder(out.raw_radians-center,2*kPi);
    out.target_radians=center+std::clamp(centered,-kTwistTargetLimit,kTwistTargetLimit);
    forearm_share=std::clamp(forearm_share,0.0,1.0);
    twist_share=std::clamp(twist_share,0.0,1.0);
    watch_share=std::isfinite(watch_share)&&watch_share>=0 ? std::min(watch_share,1.0) : forearm_share;
    const auto bounded=[](double v) { return std::isfinite(v)&&std::fabs(v)<=kPi+1e-9; };
    const bool usable=state && state->valid && bounded(state->forearm_radians) && bounded(state->twist_radians) &&
        bounded(state->watch_radians) &&
        state->seam_sign>=-1 && state->seam_sign<=1 && std::isfinite(state->seam_seconds) &&
        state->seam_seconds>=0 && state->seam_seconds<seam_window && std::isfinite(state->seam_vote) &&
        std::fabs(state->seam_vote)<=state->seam_seconds+1e-9 &&
        std::isfinite(dt) && dt>0 && dt<=.15;
    const auto wrap=[](double v) { return std::remainder(v,2*kPi); };
    if (!usable) {
        out.forearm_radians=wrap(out.target_radians*forearm_share);
        out.twist_radians=wrap(out.target_radians*twist_share);
        out.watch_radians=wrap(out.target_radians*watch_share);
        if(state) {
            // The authored-support follower owns its own continuity. A long
            // frame may reset this global principal-angle filter without
            // discarding a still-attached support hand's bounded palm path.
            const auto support=state->support;
            *state={}; state->valid=true; state->support=support;
            state->forearm_radians=out.forearm_radians; state->twist_radians=out.twist_radians;
            state->watch_radians=out.watch_radians;
            if(std::fabs(centered)>=seam_enter) state->seam_sign=centered<0?-1:1;
        }
        return out;
    }
    const int sign=centered<0?-1:1;
    if(std::fabs(centered)<seam_leave) {
        state->seam_sign=0; state->seam_seconds=state->seam_vote=0;
    } else if(state->seam_sign || std::fabs(centered)>=seam_enter) {
        if(!state->seam_sign) state->seam_sign=sign;
        // Fixed-duration voting (rather than a timer reset at every sign
        // change) also converges under tracking noise exactly at the seam.
        state->seam_seconds+=dt;
        state->seam_vote+=sign*dt;
        if(state->seam_seconds>=seam_window) {
            // A mixed window has no observable preferred branch. Use the same
            // deterministic tie for fresh and previously wound histories.
            state->seam_sign=state->seam_vote<-.25*state->seam_seconds?-1:1;
            state->seam_seconds=state->seam_vote=0;
        }
        out.seam_guarded=state->seam_sign!=sign;
        out.target_radians=center+state->seam_sign*std::min(std::fabs(centered),kTwistTargetLimit);
    }
    const auto follow=[&](double previous,double desired) {
        const double delta=std::remainder(desired-previous,2*kPi);
        const double step=std::clamp(delta*(1-std::exp(-dt/.04)),-kTwistBoneRate*dt,kTwistBoneRate*dt);
        return std::remainder(previous+step,2*kPi);
    };
    out.forearm_radians=state->forearm_radians=follow(state->forearm_radians,out.target_radians*forearm_share);
    out.twist_radians=state->twist_radians=follow(state->twist_radians,out.target_radians*twist_share);
    out.watch_radians=state->watch_radians=follow(state->watch_radians,out.target_radians*watch_share);
    return out;
}

namespace {
bool ValidTransform(const skel::Xf& t) {
    return Finite(t.translation) && ValidRotation(t.rotation) && Finite(t.scale) &&
           std::fabs(t.scale.X)>1e-6 && std::fabs(t.scale.Y)>1e-6 && std::fabs(t.scale.Z)>1e-6;
}
}
Attachment MeasureAttachment(const skel::Xf& start,const skel::Xf& end,const skel::Xf& attachment) {
    Attachment out;
    if (!ValidTransform(start) || !ValidTransform(end) || !ValidTransform(attachment)) return out;
    const FVector segment=end.translation-start.translation;
    const double square=Dot(segment,segment);
    if (!std::isfinite(square) || square<1e-6) return out;
    out.along=Dot(attachment.translation-start.translation,segment)/square;
    if (!std::isfinite(out.along) || out.along<-.001 || out.along>1.001) return out;
    out.along=std::clamp(out.along,0.0,1.0);
    const FVector off=Normalized(start.rotation).Inverse().Rotate(
        attachment.translation-start.translation-segment*out.along);
    out.off_axis_local={off.X/start.scale.X,off.Y/start.scale.Y,off.Z/start.scale.Z};
    out.local=skel::Relative(attachment,start);
    out.valid=Finite(out.off_axis_local) && ValidTransform(out.local);
    return out;
}
bool PlaceAttachment(const Attachment& r,const skel::Xf& start,const FVector& end,skel::Xf* out) {
    if (!out || !r.valid || !ValidTransform(start) || !ValidTransform(r.local) ||
        !Finite(end) || !Finite(r.off_axis_local) || !std::isfinite(r.along) || r.along<0 || r.along>1) return false;
    const FVector segment=end-start.translation;
    if (Length(segment)<1e-5) return false;
    const FVector scaled{r.off_axis_local.X*start.scale.X,r.off_axis_local.Y*start.scale.Y,
                         r.off_axis_local.Z*start.scale.Z};
    skel::Xf placed=skel::Compose(r.local,start);
    placed.translation=start.translation+segment*r.along+Normalized(start.rotation).Rotate(scaled);
    if (!ValidTransform(placed)) return false;
    *out=placed; return true;
}

}  // namespace holevr::vr::arm_pose
