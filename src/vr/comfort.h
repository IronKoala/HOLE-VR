#pragma once
// Comfort: the vignette (edges of the view darken while the stick moves or
// turns you) and fades to black (head inside a wall). Both are drawn by the
// headset compositor as one head-locked layer on top of the game picture, so
// they cost the game nothing. Settings comfort_vignette_move/_turn and
// head_wall_fade in hole-vr.ini, also on the VR tab in Options.
#include "ue/objects.h"

namespace holevr::vr::comfort {

// Before the VR session starts (registers the OpenXR layer).
void Init();
// After controls::Init(): the game tick that checks the head against walls.
void InitGame();

// From the controls tick: how much of the movement this frame is artificial
// (0..1 of full stick speed) and how fast the stick turns you (0..1 of 90
// degrees a second). Values older than a few frames count as zero, so a
// paused game never leaves the vignette on.
void SetMotion(double move01, double turn01);
// A fade to black for a moment (teleport). 0..1 opacity right now; it eases out.
void Blink(double seconds);

}  // namespace holevr::vr::comfort
