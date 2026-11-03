#pragma once
// The hole in the floor that opens the level menu at home and takes you out
// of a level (LCBP_Hole_C). The owner found it "unreliable" (2026-11-03): in
// the logs the menu came 10 to 15 s after the first press, with more presses
// in between, so the hole seems to count down while you stand in it. This
// writes what the hole does to the log (its state, how long it has waited
// out of how long, whether you are inside its area, whether you are falling)
// and gives both hands a soft tick each second while it counts, so you know
// to stay put. hole_buzz=0 in hole-vr.ini turns the ticks off.
namespace holevr::vr::hole_watch {

// After game::Init(): the game-thread tick that watches the hole.
void InitGame();

}  // namespace holevr::vr::hole_watch
