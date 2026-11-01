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
namespace holevr::vr::body {

// Registers the game tick listener. Call after controls::Init() so hand
// poses for this frame are already published.
void Init();

}  // namespace holevr::vr::body
