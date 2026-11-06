#pragma once
// World-space hand poses for other systems (wrist HUD, body IK, weapons).
// Updated on the game thread every game frame by vr/controls.

#include <cstdint>

#include "vr/math.h"
#include "vr/hand_ownership.h"

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
// kSupportGrip: left hand on a rifle's handguard (target = the point it holds,
// rotation = the hand's own); the gun turns so that point follows the hand.
enum class GrabKind { kNone, kGunGrip, kSlideGrab, kMagHeld, kMagInsert, kHolster, kSupportGrip };
struct HandGrab {
    GrabKind kind = GrabKind::kNone;
    uintptr_t object = 0;   // gun or magazine UObject
    Pose target;            // world transform the hand should take while attached
    // kSupportGrip: which grip point on the handguard (vr/interact/grip_points.h;
    // each has its own hand pose), -1 where the hand grabbed.
    // kSlideGrab: kPointChargingHandle = a rifle's charging handle held at its
    // rear end (one pose per hand), -1 = a pistol slide (holds by grab spot).
    int point = -1;
    // Visual-only reach onto a seated pistol; never acquires input ownership
    // or changes the controller. The authored approach clears the shell.
    bool holster_preview = false;
    double pose_blend = 1.0;
    // kMagInsert: how far the magazine is in, 0 with its top at the magwell
    // opening, 1 seated (plays the game's reload animation on the hand).
    double insert = 0.0;
};
// Interaction checks use the held kind; pose solvers keep the authored kind
// so the visual-only approach can still use the pistol's hand shape.
inline GrabKind HeldGrabKind(const HandGrab& pose) {
    return pose.holster_preview ? GrabKind::kNone : pose.kind;
}
// A visual holster approach is not a held item. Actual ownership and the
// attachment/drink holders still veto calibration, even if a pose is stale.
// Workbench/drink candidate reservations alone do not mean an object is held.
inline bool HasHeldObject(const HandGrab& pose, const HandInteraction& access,
                          bool attachment_held, bool drink_held) {
    return access.owner == GripOwner::kControls || attachment_held || drink_held ||
           HeldGrabKind(pose) != GrabKind::kNone;
}
constexpr int kPointChargingHandle = 0;
HandGrab GetGrab(Hand hand);
// A rifle's charging handle is grabbed at the rear end of its mesh (from
// 2026-11-04; before, at the handle's origin). shift = the grab point now
// minus the old one, gun frame, cm, so a hand pose saved on the old point
// can stay where it was on the gun. false: not measured on this gun yet.
bool ChargingHandleShift(uintptr_t gun, FVector* shift);

// Authoritative input reservation, separate from the body IK pose above.
// Includes attachment/drink holds, and remains owned during tracking loss.
// New actions must check this; existing holders may always release/clean up.
HandInteraction GetInteraction(Hand hand);

// Raw finger inputs for hand poses.
struct FingerInput {
    float trigger = 0.f;      // 0..1
    bool trigger_touch = false;
    float grip = 0.f;         // 0..1
    bool thumb_touch = false; // thumb resting on a button/stick (when the controller reports it)
};
FingerInput GetFingers(Hand hand);

// Buttons for the grip editor (vr/body/grip_editor.h), from this frame's
// controller state before anything is filtered for the game.
struct EditButtons {
    bool y_down = false;              // left Y / secondary
    bool left_stick_pressed = false;  // left stick click, went down this frame
    bool left_stick_down = false;
    bool grip_down[2] = {false, false};
};
EditButtons GetEditButtons();
// The controller the grip editor is using (kLeft/kRight), or -1. While set,
// controls ignores that controller's buttons for game actions and the gun
// does not fire.
void SetEditorHand(int hand);
int EditorHand();

// Head (centre eye) in world space; valid once the headset is rendering.
bool Head(Pose* out);
// One eye (0 left, 1 right) in world space, from the headset's own eye
// views (real eye spacing and world scale).
bool Eye(int eye, Pose* out);

}  // namespace holevr::vr::hands
