#pragma once
// Finger poses from fingertip targets, solved once and then held (the Into
// the Radius 1 approach). A finger is three joints; each bends about its own
// flex axis (toward the palm), and the base joint can also spread sideways.
// The solver searches those angles, within joint limits, for the pose whose
// tip lands closest to the target. Middle and tip joints are coupled like a
// real finger, so poses stay natural. Unreachable targets work as aim
// points. Pure math, unit tested in tests/test_body.cpp.
//
// Everything is in the hand bone's space.

#include <functional>

#include "vr/body/skeleton.h"

namespace holevr::vr::fingers {

struct Chain {
    bool valid = false;
    bool thumb = false;
    skel::Xf ref[3];        // reference local transforms: joint 0 relative to the hand, 1 to 0, 2 to 1
    FVector tip;            // fingertip in joint 2's space
    FVector flex[3];        // flex axis of each joint, in that joint's reference frame (+ = curl)
    FVector spread;         // sideways axis of the base joint, in its reference frame
    double length = 0;      // base joint to fingertip, straight
};

// joints: the finger's three joints in hand space (reference pose).
// palm: out of the palm, hand space. pinky_base: pinky knuckle (thumb curl
// direction).
Chain Build(const skel::Xf joints[3], const FVector& palm, const FVector& pinky_base, bool thumb);

struct Angles {
    double flex[3] = {0, 0, 0};  // radians
    double spread = 0;
    // Free rotation of each joint after flex/spread, in the joint's own
    // frame (the grip editor turns joints in all three axes with it).
    FQuat extra[3] = {FQuat{}, FQuat{}, FQuat{}};
};

struct Limits {
    double flex_min[3], flex_max[3];  // degrees
    double spread_min, spread_max;
    double couple;                    // joint 2 = couple x joint 1
};
Limits DefaultLimits(bool thumb);

// Local rotation of joint j (relative to its parent) for these angles.
FQuat LocalRotation(const Chain& c, int j, const Angles& a);
// Joints (hand space) and the tip for these angles.
void Fk(const Chain& c, const Angles& a, skel::Xf out[3], FVector* tip);

// How deep a hand-space point is inside the held object (0 = outside).
using DepthFn = std::function<double(const FVector&)>;

// Best angles for a fingertip target (hand space). *error: tip distance
// from the target afterwards, cm. With `depth`, poses that put the finger
// (radius `radius`) inside the object are avoided; *clip receives the
// deepest point of the result. With `start`, only refines from there (fast,
// for live editing) instead of searching all poses.
Angles Solve(const Chain& c, const FVector& target, const Limits& lim, double* error = nullptr,
             const DepthFn* depth = nullptr, double radius = 0.0, double* clip = nullptr,
             const Angles* start = nullptr);

// Deepest point of the posed finger inside the object.
double FingerDepth(const Chain& c, const Angles& a, const DepthFn& depth, double radius);

// Plain curl 0 (straight) .. 1 (closed), for free hands.
Angles Curl(const Chain& c, double amount, const Limits& lim);

Angles Blend(const Angles& a, const Angles& b, double t);

}  // namespace holevr::vr::fingers
