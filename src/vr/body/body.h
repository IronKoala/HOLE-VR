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
//   - legs: two-bone IK keeps the animated feet planted while the pelvis
//     moves;
//   - the head is shrunk to nothing so it never blocks your view.
// Clothing/hair meshes that followed BodyMesh are pointed at the copy.
//
// F9 toggles it (falls back to the game's own body). Settings: body_* in
// hole-vr.ini.
#include "vr/hands.h"

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

// Measures the arms like holding the left stick click 2 s does: a 5 s
// countdown (buzzes) once the game runs again, then stand straight with both
// arms straight out to the sides. For the VR tab in Options.
void RequestMeasure();

// Registers the game tick listener. Call after controls::Init() so hand
// poses for this frame are already published.
void Init();

}  // namespace holevr::vr::body
