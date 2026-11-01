#include "vr/body/ik_math.h"

#include <algorithm>

namespace holevr::vr::ik {

FQuat Normalized(const FQuat& q) {
    double l = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    if (l < 1e-12) return FQuat{};
    return {q.X / l, q.Y / l, q.Z / l, q.W / l};
}

FQuat AxisAngle(const FVector& axis, double radians) {
    FVector n = Normalize(axis);
    double s = std::sin(radians * 0.5);
    return {n.X * s, n.Y * s, n.Z * s, std::cos(radians * 0.5)};
}

double AngleBetween(const FVector& a, const FVector& b) {
    return std::atan2(Length(Cross(a, b)), Dot(a, b));
}

FQuat Arc(const FVector& a_in, const FVector& b_in) {
    FVector a = Normalize(a_in), b = Normalize(b_in);
    double d = Dot(a, b);
    if (d < -0.999999) {
        // Opposite directions: half turn about any perpendicular axis.
        FVector axis = Cross(a, FVector{1, 0, 0});
        if (Length(axis) < 1e-6) axis = Cross(a, FVector{0, 1, 0});
        return AxisAngle(axis, kPi);
    }
    FVector c = Cross(a, b);
    return Normalized(FQuat{c.X, c.Y, c.Z, 1.0 + d});
}

FQuat AlignFrames(const FVector& a1, const FVector& a2, const FVector& b1, const FVector& b2) {
    FQuat q1 = Arc(a1, b1);
    FVector axis = Normalize(b1);
    FVector p = Reject(q1.Rotate(a2), axis);
    FVector t = Reject(b2, axis);
    if (Length(p) < 1e-6 || Length(t) < 1e-6) return q1;
    double angle = std::atan2(Dot(Cross(p, t), axis), Dot(p, t));
    return Normalized(AxisAngle(axis, angle) * q1);
}

FQuat Scale(const FQuat& q_in, double t) {
    FQuat q = Normalized(q_in);
    if (q.W < 0) q = {-q.X, -q.Y, -q.Z, -q.W};
    double s = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z);
    if (s < 1e-9) return FQuat{};
    double angle = 2.0 * std::atan2(s, q.W);
    return AxisAngle(FVector{q.X, q.Y, q.Z}, angle * t);
}

double TwistAngle(const FQuat& q_in, const FVector& axis_in) {
    FQuat q = Normalized(q_in);
    FVector axis = Normalize(axis_in);
    double p = q.X * axis.X + q.Y * axis.Y + q.Z * axis.Z;
    double a = 2.0 * std::atan2(p, q.W);
    // Shortest way round: -pi..pi.
    if (a > kPi) a -= 2 * kPi;
    if (a < -kPi) a += 2 * kPi;
    return a;
}

FQuat ClampAngle(const FQuat& q_in, double max_radians) {
    FQuat q = Normalized(q_in);
    if (q.W < 0) q = {-q.X, -q.Y, -q.Z, -q.W};
    double s = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z);
    if (s < 1e-9) return FQuat{};
    double angle = 2.0 * std::atan2(s, q.W);
    if (angle <= max_radians) return q;
    return AxisAngle(FVector{q.X, q.Y, q.Z}, max_radians);
}

TwoBoneResult SolveTwoBone(const FVector& root, const FVector& target, double upper, double lower,
                           const FVector& pole, double max_stretch) {
    TwoBoneResult r;
    FVector to = target - root;
    double dist = Length(to);
    FVector dir = Normalize(to, Normalize(pole, FVector{0, 0, -1}));
    double reach = upper + lower;
    double min_reach = std::fabs(upper - lower) + 0.01;

    // Bend direction: the pole made perpendicular to the root->target line.
    FVector bend = Reject(pole, dir);
    if (Length(bend) < 1e-6) {
        bend = Reject(FVector{0, 0, -1}, dir);
        if (Length(bend) < 1e-6) bend = Reject(FVector{1, 0, 0}, dir);
    }
    bend = Normalize(bend);

    if (dist >= reach) {
        double end_dist = std::min(dist, reach + std::max(0.0, max_stretch));
        r.reached = dist <= reach + std::max(0.0, max_stretch) + 1e-6;
        // Straight limb; any stretch is spread over both bones.
        double k = end_dist / reach;
        r.mid = root + dir * (upper * k);
        r.end = root + dir * end_dist;
        return r;
    }
    double d = std::max(dist, min_reach);
    r.reached = dist >= min_reach;
    // Law of cosines: distance along dir to the foot of the middle joint.
    double x = (upper * upper - lower * lower + d * d) / (2.0 * d);
    double h = std::sqrt(std::max(0.0, upper * upper - x * x));
    r.mid = root + dir * x + bend * h;
    r.end = root + dir * d;
    return r;
}

HandFrame MeasureHand(const FVector& hand_pos, const FQuat& hand_rot, const FVector& index_base,
                      const FVector& middle_base, const FVector& pinky_base, bool left_hand,
                      double palm_depth_cm) {
    HandFrame f;
    FQuat inv = hand_rot.Inverse();
    FVector index = inv.Rotate(index_base - hand_pos);
    FVector middle = inv.Rotate(middle_base - hand_pos);
    FVector pinky = inv.Rotate(pinky_base - hand_pos);
    FVector across = index - pinky;
    if (Length(across) < 0.3 || Length(middle) < 1.0) return f;
    across = Normalize(across);
    FVector fwd = Normalize(Reject(middle, across));
    // Palm-down left hand: fingers +X, thumb side +Y, palm -Z, and
    // Cross(+X, +Y) = +Z, so the left palm is -Cross(fwd, across). Mirrored
    // for the right hand.
    FVector c = Cross(fwd, across);
    f.palm = left_hand ? c * -1.0 : c;
    f.across = across;
    f.middle = middle;
    f.fist = middle * 0.6 + f.palm * palm_depth_cm;
    f.valid = true;
    return f;
}

FQuat HandRotation(const HandFrame& frame, const FVector& forward, const FVector& palm) {
    return AlignFrames(frame.across, frame.palm, forward, palm);
}

}  // namespace holevr::vr::ik
