#pragma once
// Skipping the title screen.
//
// HOLE opens on a title screen, and the level (and with it the VR body) only
// starts once it is closed. The first time it comes up after launch we make
// it see-through and press its START button for the player (the same
// Blueprint event a real click runs), so the game starts with the title
// already closed. Later title screens (Return to Title) are left alone.
// title_auto_start=0 in hole-vr.ini leaves the title screen alone.
//
// The title screen is up before the player exists, so the press can't wait
// for the player's tick: it runs from the START button's own per-frame
// Blueprint Tick (through the ProcessEvent hook), on the game thread.

namespace holevr::vr::launch {

// From the init thread as soon as engine discovery is done. Starts a small
// background thread that waits for the ProcessEvent hook and then watches
// for the title screen.
void Init();

}  // namespace holevr::vr::launch
