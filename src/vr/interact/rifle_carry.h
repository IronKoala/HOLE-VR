#pragma once
// The rifle in the left hand by its handguard (foregrip) while the pistol is
// the gun in the right hand, like moving from a rifle to a sidearm.
// Pure logic (no game calls), unit tested in tests/test_rifle_carry.cpp;
// vr/controls reads the hands, asks the game to switch weapons and places the
// rifle. Design and log lines: docs/RIFLE_CARRY.md.
//
// The game has one current weapon (the one that fires; vr/controls puts it in
// the right hand) and keeps the other gun in its own spot (the rifle on the
// back). The rifle can also be held by the left hand alone, by the handguard,
// whichever gun is current; it then hangs from that hand. Let go of it when
// nothing else holds it and it goes back to its spot on the back.
//
// Moves:
//   1. Rifle in both hands, let go of the right grip: the left hand keeps it.
//   2. Right grip at the pistol holster while the rifle is out: the pistol is
//      drawn (the game switches to it). A left hand on the rifle keeps it;
//      otherwise the rifle goes on the back.
//   3. Pistol out (or holstered): the left grip on the rifle's handguard,
//      wherever the rifle is, picks it up.
//   4. Right hand empty and the right grip on the rifle's pistol grip: the
//      right hand takes the rifle (the game switches to it if the pistol was
//      out). A left hand on the handguard stays there as the support hand.
//   5. The left hand lets go (or squeezes again with grip toggle) and nothing
//      else holds the rifle: it goes back on the back.

#include "vr/hands.h"
#include "vr/math.h"

namespace holevr::vr::interact {

struct CarryInput {
    bool have_rifle = false;        // the game has a rifle (its primary weapon)
    bool rifle_current = false;     // the rifle is the game's current weapon
    bool right_has_rifle = false;   // the right hand holds the current rifle
    bool right_has_pistol = false;  // the right hand holds the current pistol
    bool left_support = false;      // both hands on the current rifle (left on the handguard)
    bool left_busy = false;         // left hand holds a slide, charging handle or magazine
    bool carrying = false;          // the left hand holds the rifle by itself
    bool switching = false;         // a weapon switch we asked the game for has not happened yet
    bool left_grip_down = false;
    bool left_grip_pressed = false;
    bool right_grip_pressed = false;
    bool right_grip_released = false;
    bool left_on_handguard = false;     // left hand in the rifle's handguard zone
    bool right_on_rifle_grip = false;   // right hand within reach of the rifle's pistol grip
    bool right_at_pistol_holster = false;  // right hand in the pistol holster's zone
    bool toggle = false;            // grip toggle: squeeze again to let go (left hand)
    bool right_toggle = false;      // gun grip toggle: the right hand lets go of the rifle on a squeeze
    bool right_puts_away = true;    // the right hand alone letting go sends the rifle to its holster
};

enum class CarryAction {
    kNone,
    kPickUp,          // left hand takes the rifle by the handguard from wherever it is
    kRightLetGo,      // both hands on the rifle and the right let go: the left keeps it
    kRightPutsAway,   // only the right hand on the rifle and it let go: the rifle goes back to its holster
    kDrawPistol,      // rifle out, right grip at the pistol holster: switch to the pistol
    kRightTakesRifle, // right hand empty, right grip on the rifle's grip
    kLetGo,           // the left hand lets go of the rifle it holds alone
};

const char* ActionName(CarryAction a);

CarryAction Decide(const CarryInput& in);

// The handguard (or magwell) zone, in the rifle's gun frame (X barrel, Y
// right, Z up) from its grip point, for the controller's grip point. Same
// shape as the two-hand support grip.
bool OnHandguard(const FVector& p, double muzzle_dist, double bore_height);

// The rifle's pose relative to the hand that holds it alone.
struct CarryHold {
    FQuat rot;               // rifle gun frame in the controller grip frame
    FVector pos;             // rifle grip point (gun-frame origin) in the controller grip frame
    FVector support_local;   // where the hand holds the rifle: gun frame, from the grip point
};

// The rifle stays exactly where it is now (it was already in the hands).
CarryHold HoldAsIs(const hands::Pose& hand, const FQuat& frame_rot, const FVector& grip_world);

// Picked up from its spot: the rifle turns to lie along the hand (barrel along
// the hand's pointing direction turned by `turn`, top up over the fist), the
// bore `fist_below_bore` cm above the fist, at the point of the handguard that
// was grabbed (kept inside the handguard).
CarryHold HoldFromGrab(const hands::Pose& hand, const FQuat& frame_rot, const FVector& grip_world, double muzzle_dist,
                       double bore_height, double fist_below_bore, const FQuat& turn);

// Picked up from its spot at one of its grip points (vr/interact/grip_points.h):
// the rifle lies along the hand as in HoldFromGrab, with that point (gun
// frame) at the hand.
CarryHold HoldAtPoint(const FVector& point, const FQuat& turn);

// Rifle gun frame and grip point in the world for the hand's pose.
void PlaceFromHand(const hands::Pose& hand, const CarryHold& h, FQuat* frame_rot, FVector* grip_world);

// Gun actor pose from its gun frame and grip point. frame_in_actor: the gun
// frame in actor space; grip_rel: the grip point in actor space.
void ActorFromFrame(const FQuat& frame_rot, const FVector& grip_world, const FQuat& frame_in_actor,
                    const FVector& grip_rel, FQuat* actor_rot, FVector* actor_loc);

// Smooth 0..1 ease (smoothstep) for blending the rifle into the hand or back
// to its spot.
double Ease(double t);
FQuat BlendRot(const FQuat& a, FQuat b, double t);

}  // namespace holevr::vr::interact
