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

    // Shoulder elevation limit. An elbow rises above the shoulder only when
    // the hand does: raised above about a third of the upper arm over the
    // shoulder, or reaching back over it (a hand behind the head points the
    // elbow up). The log of 2026-11-07 07:46 had a shouldered rifle's trigger
    // elbow 30 cm above the shoulder with the hand 6 cm above it.
    const FVector up = down*-1.0;
    const double wrist_up = Dot(to,up), wrist_forward = Dot(to,forward);
    const double raised = std::max(0.0,wrist_up-.35*in.upper_cm) +
                          std::max(0.0,std::min(wrist_up,-wrist_forward));
    // A hand on a gun (grip or support) is held with the elbow hanging: when
    // the hand is near shoulder height the elbow stays well below it, about
    // 45 deg down, rather than out at shoulder height (owner, 2026-11-07:
    // elbows rose while aiming the rifle). The cap rises with the hand.
    const double hold_cap = std::clamp(.5*wrist_up-.45*in.upper_cm,-.57*in.upper_cm,.10*in.upper_cm);
    const double elbow_cap = (in.gun_hold ? hold_cap : .10*in.upper_cm) + 1.3*raised;
    // +1 for a right arm (outward = forward x down), -1 for a left arm.
    const double side_sign = Dot(Cross(forward,down),outward) >= 0 ? 1.0 : -1.0;
    // Forearm turn range around thumb-up (pi/2): about 85 deg toward palm-up,
    // 80 toward palm-down, plus a little for the wrist's own turn and
    // tracking. Past these the shoulder must rotate the whole arm.
    constexpr double kSupinationEnd = -10*kDegToRad, kPronationEnd = 180*kDegToRad;
    constexpr double kWristBack = 70*kDegToRad, kWristPalm = 80*kDegToRad;
    constexpr double kWristThumb = 20*kDegToRad, kWristLittle = 35*kDegToRad, kWristSide = 25*kDegToRad;
    const double share = std::clamp(std::isfinite(in.shoulder_twist_share) ? in.shoulder_twist_share : 0.0, 0.0, .9);
    // A share s of a forearm turn moves to the shoulder when this weight
    // balances the .40 rest cost per radian of swivel: w = .40 s / (1 - s).
    const double share_weight = .40*share/(1-share);
    double radial_sign = 0;
    if (in.hand_frame_valid && Finite(in.radial_axis_hand) && Length(in.radial_axis_hand) > .1) {
        const double r = Dot(e3,Normalize(in.radial_axis_hand));
        if (std::fabs(r) > .3) radial_sign = r > 0 ? 1.0 : -1.0;
    }

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
    // The height cap cannot ask for an elbow lower than the arm can put it:
    // with the hand high or the arm nearly straight, every plane is over.
    // Measure from the lowest reachable elbow plus a little room instead, so
    // the cap does not pin the plane to that one point and the forearm's
    // turn range still counts (a rifle's support arm, nearly straight).
    double cap = elbow_cap;
    {
        const auto triangle = SolveTwoBone(in.shoulder,in.wrist,in.upper_cm,in.forearm_cm,zero,in.max_stretch_cm);
        const FVector along = axis*Dot(triangle.mid-in.shoulder,axis);
        const double radius = Length(triangle.mid-in.shoulder-along);
        const double lowest = Dot(along,up)-radius*Length(Reject(up,axis));
        if (std::isfinite(lowest)) cap = std::max(cap,lowest+.06*in.upper_cm);
    }
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
    bool extension_locked = distance/reach >= .995 ||
        (previous_valid && state->extension_locked && distance/reach > .97);
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
        // Wrist bend, from the forearm's direction in the hand: toward the
        // back of the hand (> 0) or the palm within the palm plane, the side
        // bend out of that plane, and the whole bend angle. An atan2 side
        // angle flips by 180 deg once a bend passes 90 deg, which made a
        // forearm arriving from the back of a folded rifle hand look like a
        // small bend (2026-11-07 run: trigger elbow out at shoulder height).
        double wrist_total = 0, wrist_side_share = 0, wrist_over = 0, legacy_side = 0;
        if (in.hand_frame_valid) {
            const FVector d = hand.Inverse().Rotate(Normalize(r.solved.end-r.solved.mid));
            const double c1 = Dot(d,e1), c2 = Dot(d,e2), c3 = Dot(d,e3);
            const double across = std::sqrt(c2*c2+c3*c3);
            r.flex = std::atan2(c2,c1);
            r.deviation = std::asin(std::clamp(c3,-1.0,1.0));
            legacy_side = std::atan2(c3,c1);
            wrist_total = std::atan2(across,c1);
            if (across > 1e-9) {
                wrist_side_share = Square(c3/across);
                // Human range: an ellipse around the straight wrist, 70 deg
                // back, 80 toward the palm, 20 toward the thumb and 35 toward
                // the little finger (25 either way without the thumb's side).
                const double back_range = c2 >= 0 ? kWristBack : kWristPalm;
                const double side_range = radial_sign == 0 ? kWristSide
                    : -radial_sign*c3 > 0 ? kWristThumb : kWristLittle;
                const double range = 1/std::sqrt(Square(c2/across/back_range)+Square(c3/across/side_range));
                wrist_over = std::max(0.0,wrist_total-range);
            }
        }
        double wrist_cost;
        if (in.joint_limits) {
            // Side bends count double: the wrist's side range is the smaller.
            // Past the human range, hand_limit_cost below adds its own cost.
            const double bend = Square(wrist_total)*(1+wrist_side_share);
            wrist_cost = in.straight_wrist ? bend : .12*bend;
        } else {
            // The solver before joint ranges (body_arm_joint_limits=0).
            const double flex_over = std::max(0.0,std::fabs(r.flex)-55*kDegToRad);
            const double side_over = std::max(0.0,std::fabs(legacy_side)-25*kDegToRad);
            const double bend = Square(r.flex)+2*Square(legacy_side);
            wrist_cost = in.straight_wrist ? bend : .12*bend+2*Square(flex_over)+4*Square(side_over);
        }
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
        double limit_cost = 0, hand_limit_cost = 0;
        r.elbow_height_cm = Dot(elbow,up);
        r.elbow_cap_cm = cap;
        r.elbow_over_cm = std::max(0.0,r.elbow_height_cm-cap);
        // The forearm's turn is read from the wrist's side-to-side axis (the
        // axis the wrist flexes about), so a bent wrist does not read as a turn:
        // with a straight wrist this is exactly the palm direction.
        if (in.hand_frame_valid) {
            const FVector f = r.solved.end-r.solved.mid;
            r.forearm_rotation = arm_pose::ForearmRotation(elbow,f,
                Cross(hand.Rotate(e3),Normalize(f,axis)),r.pole,side_sign);
        }
        if (in.joint_limits) {
            // Steep enough that no wrist or rest preference buys a raised
            // elbow: 1 cm over costs about as much as a 90 deg swivel.
            limit_cost += 1000*Square(r.elbow_over_cm/in.upper_cm);
            // A gun is held with the forearm reaching forward to it. With the
            // hand close to the shoulder, an elbow out at the side ends up
            // ahead of the hand, pointing the forearm back at the body.
            if (in.gun_hold) limit_cost += 40*Square(std::max(0.0,Dot(r.solved.mid-in.wrist,forward)/in.upper_cm));
            if (in.hand_frame_valid) {
                if (std::isfinite(r.forearm_rotation)) {
                    const double rho = r.forearm_rotation;
                    const double from_thumb_up = std::remainder(rho-kPi/2,2*kPi);
                    const double past = rho < kSupinationEnd && rho > -kPi+1e-12
                        ? std::min(kSupinationEnd-rho,rho+kPi) : std::max(0.0,rho-kPronationEnd);
                    hand_limit_cost += share_weight*Square(from_thumb_up) + 12*Square(past);
                }
                // Wrist range, for every hand: free hands and hands posed on a
                // gun alike.
                hand_limit_cost += 4*Square(wrist_over);
            }
        }
        r.swivel = angle-preferred_angle;
        r.cost = .40*Square(r.swivel) + torso_cost + hand_weight*(wrist_preference ? wrist_cost : 0)
            + limit_cost + hand_weight*hand_limit_cost
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

    // A plane carried through extension must not hold a raised elbow once
    // the arm bends again: rejoin the limited range at the usual rate. A
    // straight, slightly raised arm has its elbow over the limit on every
    // plane; only a plane worse than the best one by a centimetre is let go.
    if (extension_locked && previous_valid && in.joint_limits) {
        const Result held = evaluate(previous_unclamped);
        if (held.valid && held.elbow_over_cm > best.elbow_over_cm + 1.0) extension_locked = false;
    }
    const bool extension_recovering = previous_valid && !extension_locked &&
        (state->extension_locked || state->extension_recovering);
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
