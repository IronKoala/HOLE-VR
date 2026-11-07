#include "vr/body/arm_posture.h"

#include <algorithm>
#include <limits>

namespace holevr::vr::arm_posture {
namespace {
using namespace ik;
bool Finite(const FVector& v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}
bool Rotation(const FQuat& q) {
    const double n = q.X*q.X + q.Y*q.Y + q.Z*q.Z + q.W*q.W;
    return std::isfinite(n) && n > 1e-12;
}
double Ramp(double x, double low, double high) {
    return std::clamp((x-low)/(high-low), 0.0, 1.0);
}
double Square(double x) { return x*x; }
} // namespace

Result Solve(const Input& in, State* state) {
    Result result;
    auto invalid = [&]() { if (state) *state = {}; return Result{}; };
    if (!in.reference.valid || !Finite(in.shoulder) || !Finite(in.wrist) ||
        !Rotation(in.hand_rotation) || !Rotation(in.reference.forearm_to_hand) ||
        !Finite(in.reference.upper_axis) || !Finite(in.reference.forearm_axis) ||
        !Finite(in.reference.upper_normal) || !Finite(in.reference.forearm_normal) ||
        Length(in.reference.upper_axis) < .1 || Length(in.reference.forearm_axis) < .1 ||
        Length(in.reference.upper_normal) < .1 || Length(in.reference.forearm_normal) < .1 ||
        !Finite(in.forward) || !Finite(in.outward) || !Finite(in.down) || !Finite(in.preferred_pole) ||
        (in.torso_center_valid && !Finite(in.torso_center)) ||
        !std::isfinite(in.upper_cm) || !std::isfinite(in.forearm_cm) ||
        in.upper_cm < 1 || in.forearm_cm < 1 || in.upper_cm > 150 || in.forearm_cm > 150 ||
        !std::isfinite(in.max_stretch_cm) || in.max_stretch_cm < 0 ||
        !std::isfinite(in.dt) || in.dt < 0 || Length(in.forward) < .1) return invalid();

    const FVector forward = Normalize(in.forward);
    const FVector out_raw = Reject(in.outward, forward);
    if (Length(out_raw) < .1) return invalid();
    const FVector outward = Normalize(out_raw);
    const FVector down_raw = Reject(Reject(in.down, forward), outward);
    if (Length(down_raw) < .1) return invalid();
    const FVector down = Normalize(down_raw);
    const double reach = in.upper_cm + in.forearm_cm;
    const FVector to = in.wrist-in.shoulder;
    const double distance = Length(to);
    if (!std::isfinite(distance)) return invalid();
    const FVector axis = Normalize(to, forward);
    const FQuat hand = Normalized(in.hand_rotation);
    FVector e1, e2, e3;
    if (in.hand_frame_valid) {
        if (!Finite(in.forearm_axis_hand) || !Finite(in.palm_axis_hand) || Length(in.forearm_axis_hand) < .1)
            return invalid();
        e1 = Normalize(in.forearm_axis_hand);
        e2 = Reject(in.palm_axis_hand, e1);
        if (Length(e2) < .1) return invalid();
        e2 = Normalize(e2);
        e3 = Cross(e1, e2);
    }

    const double above = std::max(0.0, -Dot(to,down))/reach;
    const double below = std::clamp(Dot(to,down)/reach, 0.0, 1.0);
    const double cross_body = Ramp(-Dot(axis,outward), .20, .80);
    const double overhead = Ramp(above, .35, .85);
    const double relaxed = std::max(cross_body, overhead);
    // Resting bends favour the side of the body. A large outward pole used
    // to flare a half-reached elbow by almost half an upper-arm length.
    // This is a preference only: the exact two-bone triangle still owns reach.
    const FVector tight = down*(1-.6*above) + outward*(.18+.6*above-.10*below)
                          - forward*(.10+.45*below);
    // With a folded hand low beside the body, aggressively reducing lateral
    // spread can simply rotate the same elbow circle behind the back. Retain
    // room for that bend rather than claiming a tighter lateral coordinate
    // is always a better posture. The medial cost below still applies.
    const double low_fold = Ramp(below,.15,.40);
    const FVector low_bend = down*(1-.6*above) + outward*(.7+.6*above-.5*below)
                             - forward*(.3+1.5*below);
    const FVector natural = tight+(low_bend-tight)*low_fold;
    const double lateral_reach = Ramp(Dot(to,outward)/reach,.15,.70);
    const double tuck_weight = (1-lateral_reach)*(1-overhead)*(1-low_fold);

    // Use the projected outward direction as the zero of a single angular
    // interval. The neutral forward interval is the outward half-circle.
    // Cross-body and overhead reaches may use some medial bend, without
    // changing their wrist target or pretending the arm is shorter.
    const FVector out_projection = Reject(outward,axis);
    const double out_length = Length(out_projection);
    FVector zero = out_length > .05 ? out_projection*(1/out_length) : Reject(natural,axis);
    if (Length(zero) < .05) zero = Reject(down,axis);
    if (Length(zero) < .05) zero = Reject(forward,axis);
    if (Length(zero) < .05) return invalid();
    zero = Normalize(zero);
    const FVector tangent = Normalize(Cross(axis,zero));
    auto angle_of = [&](const FVector& pole) {
        return std::atan2(Dot(pole,tangent), Dot(pole,zero));
    };
    auto pole_at = [&](double angle) { return zero*std::cos(angle)+tangent*std::sin(angle); };

    // With the arm pointing out sideways, the elbow is already well away
    // from the torso and outward projected onto its plane approaches zero.
    // Relax that plane bound before the singularity; otherwise crossing a
    // T-pose flips the sign of a tiny projection and snaps the straight arm.
    const double plane_relaxed = std::max(relaxed,Ramp(Dot(axis,outward),.55,.85));
    double threshold = -.80*plane_relaxed;
    bool clearance_limited = false;
    if (in.torso_center_valid && out_length > .05) {
        const auto triangle = SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,zero,in.max_stretch_cm);
        const FVector along = axis*Dot(triangle.mid-in.shoulder,axis);
        const double bend_radius = Length(triangle.mid-in.shoulder-along);
        // Guard the actual elbow against the midline for ordinary forward
        // reaches. Relax for crossed/overhead hands, where medial elbows can
        // be necessary. If exact geometry cannot satisfy this, choose the
        // furthest-out feasible plane and report it; never move the wrist.
        const double minimum = std::min(3.0,in.upper_cm*.10)*(1-relaxed)-reach*.15*relaxed;
        const double centre = Dot(in.shoulder+along-in.torso_center,outward);
        if (bend_radius*out_length > 1e-5) {
            const double required = (minimum-centre)/(bend_radius*out_length);
            clearance_limited = required > 1 && relaxed < .05;
            // A crossed arm can require an elbow on the opposite side of
            // the shoulder. Fade this actual-position guard with that reach
            // instead of pinning an impossible elbow to a vanishing outward
            // projection; torso proximity remains part of the soft cost.
            const double guard = std::clamp(required,-.8,1.0)*(1-relaxed)+threshold*relaxed;
            threshold = std::max(threshold,guard);
        } else clearance_limited = centre < minimum;
    }
    const double half = out_length > .05 ? std::acos(std::clamp(threshold,-.8,1.0)) : 110*kDegToRad;
    // Optional authored-pose preferences fade at the straight-arm
    // singularity. Free hand orientation is used only for diagnostics/twist.
    const double hand_weight = 1-Ramp(distance/reach,.90,.995);
    const bool wrist_preference = in.hand_frame_valid && (in.wrist_relief || in.straight_wrist);
    const FVector configured = Length(in.preferred_pole) > .05
        ? natural+(in.preferred_pole-natural)*hand_weight : natural;
    FVector preferred = Reject(configured,axis);
    if (Length(preferred) < .05) preferred = Reject(natural,axis);
    if (Length(preferred) < .05) preferred = zero;
    const double preferred_angle = std::clamp(angle_of(preferred),-half,half);
    // Rest style belongs in the cost, not another hard angular window.
    // Its projected direction can cross atan2's branch during a sweep; a
    // preference-centred window then ejects a still-admissible prior elbow
    // and defeats the rotation-rate limit. Search the connected body domain.
    const double low = -half, high = half;
    result.preferred_pole = pole_at(preferred_angle);

    bool previous_valid = state && state->valid && Finite(state->axis) && Finite(state->pole) &&
        Length(state->axis) > .5 && Length(state->pole) > .5 && in.dt <= .25 &&
        Dot(Normalize(state->axis),axis) > -.95;
    double previous_angle = preferred_angle;
    double previous_unclamped = preferred_angle;
    if (previous_valid) {
        const FVector previous = Arc(state->axis,axis).Rotate(state->pole);
        previous_unclamped = angle_of(previous);
        previous_angle = std::clamp(previous_unclamped,low,high);
    }
    const bool extension_locked = distance/reach >= .995 ||
        (previous_valid && state->extension_locked && distance/reach > .97);
    const bool extension_recovering = previous_valid && !extension_locked &&
        (state->extension_locked || state->extension_recovering);
    // At extension the plane cannot be observed from the triangle. Do not
    // rotate the entire straight arm merely to chase hand roll or tiny noise.
    auto evaluate = [&](double angle) {
        Result r;
        r.pole = pole_at(angle);
        r.preferred_pole = result.preferred_pole;
        r.solved = SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,r.pole,in.max_stretch_cm);
        r.rotations = arm_pose::Orient(in.reference,in.shoulder,r.solved,r.pole);
        if (!r.rotations.valid) return r;
        r.raw_roll = arm_pose::WristTwist(in.reference,r.rotations.forearm,hand);
        if (in.hand_frame_valid) {
            const FVector d = hand.Inverse().Rotate(Normalize(r.solved.end-r.solved.mid));
            r.flex = std::atan2(Dot(d,e2),Dot(d,e1));
            r.deviation = std::atan2(Dot(d,e3),Dot(d,e1));
        }
        const double flex_over = std::max(0.0,std::fabs(r.flex)-55*kDegToRad);
        const double side_over = std::max(0.0,std::fabs(r.deviation)-25*kDegToRad);
        const double bend = Square(r.flex)+2*Square(r.deviation);
        const double wrist_cost = in.straight_wrist ? bend : .12*bend+2*Square(flex_over)+4*Square(side_over);
        // Roll is not evidence for the elbow plane. Let the hand and skin
        // twist follow it instead of moving the elbow to reduce a frame angle.
        const FVector elbow = r.solved.mid-in.shoulder;
        const double elbow_forward = Dot(elbow,forward)/in.upper_cm;
        const double elbow_medial = std::max(0.0,-Dot(elbow,outward)/in.upper_cm-.04);
        const double elbow_flare = std::max(0.0,Dot(elbow,outward)/in.upper_cm-.15);
        // Moving forward is not permission to drift inward across the chest.
        // Cross-body targets already constrain the feasible elbow circle;
        // prefer its less medial solutions rather than treating an inward
        // hand as permission for arbitrary extra elbow crossing. Raised
        // reaches ease this soft preference without pinning a fixed point.
        // Reaching out to the side releases the tuck cost, not the wrist target.
        const double torso_cost = 12*Square(elbow_medial)*(1-.25*overhead)
                                  +2*Square(elbow_flare)*tuck_weight
                                  +2*Square(std::max(0.0,-elbow_forward-.15));
        r.swivel = angle-preferred_angle;
        r.cost = .40*Square(r.swivel) + torso_cost + hand_weight*(wrist_preference ? wrist_cost : 0)
            +(previous_valid ? .06*Square(angle-previous_angle) : 0);
        r.torso_clearance_cm = in.torso_center_valid ? Dot(r.solved.mid-in.torso_center,outward) : 0;
        r.valid = std::isfinite(r.cost) && std::isfinite(r.raw_roll) &&
                  std::isfinite(r.flex) && std::isfinite(r.deviation);
        return r;
    };

    double best_angle = preferred_angle;
    Result best;
    best.cost = std::numeric_limits<double>::infinity();
    unsigned count = 0;
    auto consider = [&](double angle) {
        if (angle < low-1e-9 || angle > high+1e-9) return;
        Result r = evaluate(std::clamp(angle,low,high));
        ++count;
        if (r.valid && r.cost < best.cost) { best = r; best_angle = std::clamp(angle,low,high); }
    };
    consider(preferred_angle);
    if (previous_valid) consider(previous_angle);
    const int intervals = std::clamp(static_cast<int>(std::ceil((high-low)/(5*kDegToRad))),1,54);
    for (int i=0;i<=intervals;++i) consider(low+(high-low)*i/intervals);
    const auto wrist_aligned = wrist_preference
        ? arm_pose::WristAlignedPole(in.reference,in.shoulder,in.wrist,hand) : arm_pose::WristPole{};
    const double aligned_angle = wrist_aligned.valid ? angle_of(wrist_aligned.pole) : preferred_angle;
    if (wrist_aligned.valid && hand_weight > 0) consider(aligned_angle);
    const double coarse = best_angle;
    for (int i=-2;i<=2;++i) consider(coarse+i*kDegToRad);
    if (!best.valid) return invalid();

    double final_angle = best_angle;
    if (previous_valid && extension_locked) {
        // The bend-plane direction has no geometric evidence at extension.
        // Parallel transport keeps its roll instead of chasing a changing
        // body preference or a tiny projected outward vector. Hysteresis
        // avoids toggling that decision as tracking hovers around full reach.
        final_angle = previous_unclamped;
    } else if (extension_recovering) {
        // Turning a straight arm can transport the stored plane outside the
        // current body's preferred interval. Rejoining it is not new elbow
        // evidence: spend the same angular-rate budget rather than clamping
        // a 34-degree jump into the first retracted frame.
        const double difference = std::remainder(best_angle-previous_unclamped,2*kPi);
        const double maximum = 360*kDegToRad*std::min(in.dt,.25);
        final_angle = previous_unclamped+std::clamp(difference*(1-std::exp(-in.dt/.08)),-maximum,maximum);
    } else if (previous_valid) {
        const double difference = (best_angle-previous_angle)*(1-std::exp(-in.dt/.08));
        const double maximum = 360*kDegToRad*std::min(in.dt,.25);
        // Count movement back into a changing domain against the same rate
        // budget. Clamping first and then adding a full rate step overshoots.
        const double rate_low = std::max(low,previous_unclamped-maximum);
        const double rate_high = std::min(high,previous_unclamped+maximum);
        final_angle = rate_low <= rate_high
            ? std::clamp(previous_angle+difference,rate_low,rate_high)
            : previous_angle; // abrupt/invalid old pose: admissibility wins
    }
    result = evaluate(final_angle);
    if (!result.valid) return invalid();
    result.candidates = count;
    result.constrained = clearance_limited || final_angle < low+2*kDegToRad || final_angle > high-2*kDegToRad ||
        (wrist_aligned.valid && (aligned_angle < low || aligned_angle > high));
    const double solved_angle = angle_of(result.pole);
    const bool recovering = extension_recovering && (solved_angle<low-1e-8 || solved_angle>high+1e-8);
    if (state) *state = {true,axis,result.pole,extension_locked,recovering};
    return result;
}

} // namespace holevr::vr::arm_posture
