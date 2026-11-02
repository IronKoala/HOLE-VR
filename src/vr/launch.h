#pragma once
// Launching straight into the game in VR.
//
// HOLE opens on a title screen drawn over the live level. Its menu is a flat
// 2D widget that needs the keyboard, so in the headset it is a box you can't
// use. Once the headset is showing the game, we press its START button for
// the player (the same Blueprint event a real click runs), so launching the
// game puts you in the level with nothing to do at the PC.
// title_auto_start=0 in hole-vr.ini leaves the title screen alone.

namespace holevr::vr::launch {

// Settings. From the VR start-up thread.
void Init();

// From the init thread's loop (about 60 times a second). Finds the title
// screen and hands the button press to the game thread.
void Poll();

}  // namespace holevr::vr::launch
