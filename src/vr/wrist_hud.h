#pragma once

// Wrist display and HUD split.
//
// HOLE's flat HUD can't work in a headset (it is drawn flat over the
// window), so the parts you need during play move to a small watch-like
// display on the off-hand wrist that appears when you turn the wrist toward
// your face: rounds in the magazine and chamber, jam/slide-lock state with a
// hint, and health. The values are read from the game, not from the HUD
// image. The matching flat HUD parts (crosshair, ammo, reload hint, barrel
// heat, health) are hidden.
//
// It also logs when each of the game's HUD parts shows or hides, so the
// remaining parts (prompts, pickups, objectives, money) can be placed once
// we've seen when the game uses them.
//
// Low health also pulses both controllers like a heartbeat, so it can be
// felt without looking.
namespace holevr::vr::wrist_hud {
// Before the VR session starts (registers with OpenXR).
void Init();
// After game::Init(): starts reading the game's state each tick.
void InitGame();
}  // namespace holevr::vr::wrist_hud
