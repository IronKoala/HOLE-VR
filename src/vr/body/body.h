#pragma once
// Full-body avatar in VR.
//
// The game's own BodyMesh keeps running its animation (ALS locomotion, the
// game camera stays on its head), but is hidden. A PoseableMeshComponent
// with the same mesh and materials copies that pose every frame and we then
// rewrite the parts VR owns:
//   - body yaw follows the headset (with a dead zone), pelvis drops when you
//     crouch, the spine leans toward where your head really is;
//   - arms: two-bone IK from the shoulders to the controllers, snapping the
//     hand to the gun grip / slide / magazine while something is held;
//   - fingers: hand poses from vr/body/hand_pose (grip, trigger, slide,
//     magazine) and the controller's finger sensors;
//   - legs: reference-frame solving preserves moving strides; grounded idle
//     feet plant/step on verified floor. Kick/airborne animation owns the legs;
//   - the head is shrunk to nothing so it never blocks your view.
// Clothing/hair meshes that followed BodyMesh are pointed at the copy.
//
// F9 toggles it (falls back to the game's own body). Settings: body_* in
// hole-vr.ini.
#include "vr/hands.h"
#include "vr/watch_math.h"

namespace holevr::vr::body {

// Gear slots on the VR body, for holsters and pouches. They ride on the
// IK'd body (pelvis for the belt, upper spine for chest/back), so they
// crouch, lean and turn with it. Rotation: X = body forward, Y = body
// right, Z = up. Updated every game frame after the body pose (body tick
// runs after controls, so controls reads last frame's values).
enum class Slot : int {
    kRightHip,      // pistol holster
    kLeftPouch1,    // belt mag pouches, front to back
    kLeftPouch2,
    kLeftPouch3,
    kChest,         // rifle sling / chest rig
    kBack,          // rifle on the back
    kCount
};
// False while the VR body is off or not yet posed.
bool GetSlot(Slot slot, hands::Pose* out);

// The body parts gear hangs from, for holsters that keep their own offsets
// (vr/interact/holster.h; same order as interact::HolsterAnchor):
//   kBelt  = pelvis bone, the body's yaw only (X forward, Y right, Z up).
//   kChest = top spine bone, tilting with the torso (Z from the pelvis up to
//            it, X toward the body's front).
//   kThighL/kThighR = middle of the thigh, swinging with the leg (Z up along
//            the thigh, X toward the body's front). With the legs off they
//            hang from the belt.
// Same frame timing and validity as GetSlot.
enum class Anchor : int { kBelt, kChest, kThighL, kThighR, kCount };
bool GetAnchor(Anchor anchor, hands::Pose* out);

// The held pistol's slide, measured with line traces against the slide's
// own parts (once per gun, while the right hand holds it): the rear sight
// (highest point of the slide's rear 3.5 cm, on its centre line) and the top
// of the slide's rear and front ends, in world space for where the slide is
// now. False until measured (or if the gun has no traceable slide).
bool SlideMarks(uintptr_t gun, FVector* rear_sight, FVector* rear_end, FVector* front_end);

// Where the visible empty hand pinches: its thumb tip and the index finger's
// middle knuckle, relative to the controller's grip pose (hands::HandState
// grip: location + rotation), as drawn on the body's last frame with that
// hand empty. False when the body is off, the hand isn't posed, or nothing
// newer than 0.5 s.
bool PinchPoint(int side, FVector* thumb_in_grip, FVector* index_in_grip);

// What the visible hand can touch things with, in the world, as drawn on the
// body's last frame: after empty hands stop at walls, fingers give way on
// surfaces and the palm settles flat (hand_walls, finger_world, finger_contact). For
// pressing buttons with the hand the player sees, not the controller.
struct HandContacts {
    FVector tip[5];            // fingertips: thumb, index, middle, ring, pinky
    double tip_radius = 0.5;   // finger thickness about each tip, cm
    FVector palm;              // middle of the palm, on its skin side
    FVector palm_out;          // out of the palm
    bool empty = true;         // the hand holds nothing
};
// False when the body is off, the hand's fingers aren't posed, or nothing
// newer than 0.1 s. side: 0 left, 1 right.
bool GetHandContacts(int side, HandContacts* out);

// Game-thread accessory mount: the current solved forearm/wrist and their
// reference frames. False after an incomplete body tick or stale tracking.
bool GetForearm(int side, watch::Forearm* out);

// Guided height -> arms forward -> arms out -> bend once -> forward verification.
// Only quality-checked measurements replace the previous fit. Hands must be free.
void RequestMeasure();
bool CalibrationActive();
void CancelCalibration();
// Queue height-only completion (left X), including during the height step.
// Finish after height is measured; preserve the existing arm fit. Limited
// reach must never be interpreted as shorter anatomical arm segments.
void SkipCalibrationArms();
// Calibration owns both index triggers. Holding both starts the actual 2s
// measurement; either release cancels it. Both must be up before rearming.
// interrupted preserves a coalesced release/repress between body samples.
void SetCalibrationTriggers(bool left_down, bool right_down, bool valid, bool interrupted = false);
// Thread-safe text for the headset overlay. No engine access on the render thread.
enum class CalibrationPose { None, Height, SeatedHeight, Forward, Out, BendUp, Bent };
struct CalibrationHud {
    bool visible = false;
    CalibrationPose pose = CalibrationPose::None;
    char title[64] = {};
    char line[3][96] = {};
    char footer[96] = {};
    int progress = 0;  // 0..100, current representative pose window
};
CalibrationHud GetCalibrationHud();

// Registers the game tick listener. Call after controls::Init() so hand
// poses for this frame are already published.
void Init();

}  // namespace holevr::vr::body
