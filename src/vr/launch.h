#pragma once
// Skipping the title screen.
//
// HOLE opens on a title screen drawn over the live level, and the VR body only
// shows once it is closed. The first time it comes up after launch we make it
// see-through and press its START button for the player (the same Blueprint
// event a real click runs), so the game starts with the title already closed.
// Later title screens (Return to Title) are left alone.
// title_auto_start=0 in hole-vr.ini leaves the title screen alone.

namespace holevr::vr::launch {

// Settings. From the init thread, before Poll.
void Init();

// From the init thread's loop (about 60 times a second). Finds the title
// screen and hands the button press to the game thread.
void Poll();

}  // namespace holevr::vr::launch
