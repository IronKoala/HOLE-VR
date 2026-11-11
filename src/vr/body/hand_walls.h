#pragma once
// Empty hands stop at walls (owner's feature list, 2026-11-03, item 12:
// "physics hands that stop at walls").
//
// The empty hand follows tracking through a bounded damped virtual spring.
// A rig-calibrated oriented palm box resolves contact and slides tangentially;
// a grip-centred sphere is used only without valid rig geometry. Rotation and
// palm seating have checked clearance. Tracking loss/teleport reacquisition
// sweeps from the head before placement. The actual controller is untouched.
// Held objects, interaction reservations and the grip editor bypass this
// proxy so their authoritative poses keep control. No Chaos body or prop
// impulse is created by this kinematic collision-constrained spring model.
//
// A fresh forward sphere trace supplies the optional legacy finger path.
// Individual finger geometry is queried separately after arm posing.
//
// Settings: hand_wall_collision (1 = on), hand_wall_radius_cm (ball radius),
// hand_wall_probe_cm (fingertip probe length past the fist, 0 = off).
#include "ue/objects.h"
#include "vr/hands.h"
#include "vr/body/hand_motion.h"
#include "vr/body/hand_rigid_contact.h"
#include <vector>

namespace holevr::vr::body::hand_walls {

using ue::UObject;

void LoadSettings();
bool Enabled();
// Invalidate physical history and contact planes on lifecycle changes. A
// negative side resets both hands and the frame-local ignored actor list.
void Reset(int side = -1);

// Populated by Apply, reused by individual finger traces later in the same
// body frame. Contains player, validated attached actors, held objects and UI,
// and the player's own gear (SetOwnGear).
const std::vector<UObject>& IgnoredActors();
// The player's guns (held or put away), their magazines and everything
// fitted to them (controls' list, set every frame). These have no hit boxes
// but do have visible shapes, so with wall_shape=nearest or visible the hand
// stopped at the rifle on the back and at the held gun's own parts (owner
// 2026-11-08). Only live ones are used.
void SetOwnGear(const std::vector<UObject>& actors);
hand_motion::Result Motion(int side);
// Rebuilt by body from FreeHand and rig landmarks before Apply. Extents include
// the full palm volume; the palm-facing surface is centre + rotated Z * half.Z.
void SetPalmShape(int side, const hand_motion::PalmShape& shape);
// Rigid finger backpressure/final IK guard. requestedGrip must represent the
// actual drawn wrist mapping. Re-solve the arm if the checked pose differs;
// never move only the hand bone. Invalid means retain a previously verified
// pose or suppress this draw, not accept requestedGrip. Shares Apply's budget.
hand_motion::Result ConstrainFinal(int side, const hands::Pose& requestedGrip);
// After accepting the final wrist+finger geometry, remember its world-space
// witnesses in checked grip-local coordinates. Ages are the ORIGINAL query
// ages, not zeroed because a cached contact was reused. Max80; count0 clears.
// Constraints expire after only the remainder of that50ms observation window.
void SetRigidSupports(int side, const hand_rigid_contact::Plane* planes, const double* age_seconds, int count);

// hs: this frame's local body hand copies. The proxy's translation/rotation
// are applied consistently to grip, aim, wrist and palm direction.
void Apply(UObject player, const hands::Pose& head, hands::HandState hs[2], const hands::HandGrab grab[2],
           double dt);

// The surface an empty hand (0 left, 1 right) last stopped against: a point
// on it and its normal (out of the surface, toward the hand). Requires a
// fresh current-frame trace, expires after 100 ms if Apply stops, and checks
// actor lifetime and a finite local neighborhood. False otherwise.
bool Contact(int side, FVector* point, FVector* normal);

// Radius of the hand's ball, cm: the real fist centre is this far off a
// surface when the drawn hand first stops at it.
double Radius();

// The hand is touching something this frame: its ball is stopped, or its
// fingertips have reached a surface.
bool Touching(int side);

}  // namespace holevr::vr::body::hand_walls
