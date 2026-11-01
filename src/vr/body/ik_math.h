#pragma once
// Pure math for body IK and hand poses: vector helpers, rotations between
// directions and frames, and an analytic two-bone (shoulder-elbow-wrist or
// hip-knee-ankle) solver. No engine access, unit tested in tests/test_body.cpp.
//
// All rotations act on vectors with FQuat::Rotate (v' = q v q*), the same
// convention as UE's FQuat. "Toward" below means numerically: rotating `from`
// about Cross(from, to) by a positive angle moves it toward `to`.

#include "vr/math.h"

namespace holevr::vr::ik {

inline double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
inline double Length(const FVector& v) { return std::sqrt(Dot(v, v)); }
inline FVector Normalize(const FVector& v, const FVector& fallback = FVector{1, 0, 0}) {
    double l = Length(v);
    return l > 1e-9 ? v * (1.0 / l) : fallback;
}
// Component of v perpendicular to unit vector n.
inline FVector Reject(const FVector& v, const FVector& n) { return v - n * Dot(v, n); }

FQuat Normalized(const FQuat& q);
FQuat AxisAngle(const FVector& axis, double radians);  // axis need not be unit
double AngleBetween(const FVector& a, const FVector& b);  // radians, 0..pi

// Shortest rotation taking direction a onto direction b.
FQuat Arc(const FVector& a, const FVector& b);

// Rotation taking a1 exactly onto b1 and a2 as close as possible to b2
// (a2/b2 are only used for the twist about b1).
FQuat AlignFrames(const FVector& a1, const FVector& a2, const FVector& b1, const FVector& b2);

// Fraction t of rotation q (slerp from identity).
FQuat Scale(const FQuat& q, double t);

// Signed angle (radians) of the part of q that turns about unit `axis`
// (swing-twist decomposition).
double TwistAngle(const FQuat& q, const FVector& axis);

// Limits the angle of q to max_radians, keeping its axis.
FQuat ClampAngle(const FQuat& q, double max_radians);

struct TwoBoneResult {
    FVector mid;       // elbow / knee
    FVector end;       // wrist / ankle actually reached
    bool reached = true;
};

// root: shoulder/hip. target: where the end should go. upper/lower: bone
// lengths. pole: rough direction the middle joint should bend toward (it is
// projected perpendicular to the root->target line). max_stretch: how far
// past full reach the end may still follow the target (cm), 0 = none.
TwoBoneResult SolveTwoBone(const FVector& root, const FVector& target, double upper, double lower,
                           const FVector& pole, double max_stretch = 0.0);

// The avatar's hand measured in the hand bone's own space, from where its
// finger bases sit. across: pinky base -> index base (toward the thumb side
// of a fist). palm: out of the palm. fist: centre of the closed fist.
struct HandFrame {
    bool valid = false;
    FVector across, palm, fist;
};
HandFrame MeasureHand(const FVector& hand_pos, const FQuat& hand_rot, const FVector& index_base,
                      const FVector& middle_base, const FVector& pinky_base, bool left_hand,
                      double palm_depth_cm = 3.0);

// World rotation for the hand bone so the avatar's fist matches a
// controller: `forward` runs through the fist from the pinky side to the
// index side (the OpenXR grip axis), `palm` is out of the player's palm.
FQuat HandRotation(const HandFrame& frame, const FVector& forward, const FVector& palm);

}  // namespace holevr::vr::ik
