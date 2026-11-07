#pragma once
// The hub's weapon workbench in VR (LCBP_WeaponCustom_C): park a weapon on a
// table, pull attachments off it by hand, take the attachments you own off
// the table and put them on the gun. Every change goes through the game's own
// attachment system (game/weapon_custom.h: saved list + the gun rebuilding
// its parts). Design and log lines: docs/WORKBENCH.md.

#include "ue/objects.h"
#include "vr/math.h"
#include "vr/workbench_pose.h"

namespace holevr::vr::workbench {

// Registers the game tick listener. Call after controls::Init(), so the
// workbench sees the hands and the grip presses controls saw this frame.
void Init();

// --- Called by vr/controls (pistol holster and long-gun carry) ---

// The gun rests on the bench: `spot` is where a hand grabs it (world cm) and
// `frame` its gun frame in the world (X barrel, Y right, Z up). False when
// this gun is not parked. Either output may be null for a state query.
bool ParkedGun(ue::UObject gun, FVector* spot, FQuat* frame);
// The last hand let go of the selected weapon. `frame` is the
// gun frame in the world at that moment and `hand` the hand's grab point.
// `native_frame` is the gun frame in actor space, measured by controls. It
// lets the vise measure current components independently of the held pose.
// True if the workbench laid it on a table under the hand (controls leaves
// it there instead of sending it back to the holster).
// The actual releasing hand determines the vise direction: right -> barrel
// left, left -> barrel right, as viewed from the player-facing bench edge.
bool TryPark(ue::UObject gun, const FQuat& frame, const FQuat& native_frame,
             const FVector& hand, const FVector& hand_to_main,
             ReleaseHand released_by);
// Either hand picked the parked weapon up off the table.
void GunTaken();
// Candidate query for controls' per-frame ownership resolver (0 left, 1 right): the hand
// holds an attachment, or is at one and closer to it than to anything
// controls would grab. Controls ignores that grip for its own grabs.
bool ClaimsGrip(int hand);
// Actual hold only: never searches for a nearby part or claims another feature.
bool HoldsGrip(int hand);

}  // namespace holevr::vr::workbench
