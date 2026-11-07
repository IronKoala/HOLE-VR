#pragma once
// Finger curl for each hand from what it is holding and the controller's
// finger sensors. Pure logic, unit tested in tests/test_body.cpp.
//
// Curl is 0 (straight, the skeleton's reference pose) to 1 (closed fist),
// per finger. vr/body turns it into joint rotations.

#include "vr/hands.h"

namespace holevr::vr::hand_pose {

enum Finger : int { kThumb = 0, kIndex, kMiddle, kRing, kPinky, kFingerCount };

struct Curl {
    float finger[kFingerCount] = {0, 0, 0, 0, 0};
};

// The pose a hand should take right now, before smoothing.
//   kNone / kHolster: free hand, follows trigger (index), grip (other
//     three) and thumb rest.
//   kGunGrip: wrapped around the pistol grip; index straight along the frame
//     until the trigger is touched, then on the trigger, pulling with it.
//   kSlideGrab: claw grip pinching the slide serrations.
//   kMagHeld / kMagInsert: fingers around the magazine body, index along
//     its front edge, thumb on the back.
Curl Target(hands::GrabKind grab, const hands::FingerInput& in);

// Moves `current` toward `target`, exponentially, `speed` per second
// (about 1/speed seconds to get most of the way there).
void Smooth(Curl* current, const Curl& target, double dt, double speed = 18.0);

// Maximum bend of each joint (base, middle, tip) at curl 1, degrees.
struct JointLimits {
    double base, middle, tip;
};
JointLimits Limits(Finger f);

}  // namespace holevr::vr::hand_pose
