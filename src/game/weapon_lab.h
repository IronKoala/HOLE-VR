#pragma once

// Flat-screen test bench for VR weapon handling. Lets the owner try the
// game-side half of manual reloads and slide racking with hotkeys before the
// headset build exists, and logs exactly what the game does so the VR hand
// interactions can be mapped onto it.
//
//   F5        start/stop tracing (logs weapon events and ammo changes)
//   F6 (hold) pull the slide back by hand; release early = chamber check,
//             release at the end = full rack
//   F7        magazine release (mag drops out)
//   F8        take a new magazine and insert it
//   Ctrl+F6   slide catch release (drops a locked-open slide)
//   Ctrl+F7   pull the magazine out by hand instead of dropping it
//   Shift+F8   insert using the game's other load function
namespace holevr::game::weapon_lab {

// Registers the tick listener and ProcessEvent tracing. Call once after game::Init().
void Init();
// Poll hotkeys. Call from the background thread about every 16 ms.
void PollHotkeys();

}  // namespace holevr::game::weapon_lab
