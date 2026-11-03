#pragma once
// Teleport movement (locomotion=teleport in hole-vr.ini, or Moving: Teleport
// on the VR tab). Push the movement stick forward: an arc of beads comes out
// of that hand and a flat disc shows where you will land. Let go of the
// stick to go there (a quick blink to black). Pull the stick back to cancel.
#include "ue/objects.h"
#include "vr/input.h"

namespace holevr::vr::teleport {

// From the controls tick, each frame teleport mode is on and no menu is up.
// `hand` is the movement stick's hand (input::kLeft normally).
void Update(ue::UObject player, const input::HandInput& stick, int hand);
// Hides the arc (teleport off, a menu opened, no player).
void Hide();

}  // namespace holevr::vr::teleport
