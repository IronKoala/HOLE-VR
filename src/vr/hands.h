#pragma once
// World-space hand poses for other systems (wrist HUD, body IK, weapons).
// Updated on the game thread every game frame by vr/controls.

#include <cstdint>

#include "vr/math.h"

namespace holevr::vr::hands {

enum Hand : int { kLeft = 0, kRight = 1 };

struct Pose {
    FVector location;  // world, cm
    FQuat rotation;    // world
};

struct HandState {
    bool valid = false;     // tracked this frame and mapped into the world
    Pose grip;              // centre of the closed fist; UE axes: +X along the controller's pointing direction
    Pose aim;               // pointing ray: +X forward
    Pose wrist;             // back of the wrist, for a wrist HUD
    FVector palm_normal;    // world direction out of the palm
    double time = 0.0;      // seconds since the mod started, game clock
};

HandState Get(Hand hand);

// What each hand is holding, so body IK can snap the hand to the object
// instead of the raw controller.
enum class GrabKind { kNone, kGunGrip, kSlideGrab, kMagHeld, kMagInsert, kHolster };
struct HandGrab {
    GrabKind kind = GrabKind::kNone;
    uintptr_t object = 0;   // gun or magazine UObject
    Pose target;            // world transform the hand should take while attached
};
HandGrab GetGrab(Hand hand);

// Raw finger inputs for hand poses.
struct FingerInput {
    float trigger = 0.f;      // 0..1
    bool trigger_touch = false;
    float grip = 0.f;         // 0..1
    bool thumb_touch = false; // thumb resting on a button/stick (when the controller reports it)
};
FingerInput GetFingers(Hand hand);

// Head (centre eye) in world space; valid once the headset is rendering.
bool Head(Pose* out);

}  // namespace holevr::vr::hands
