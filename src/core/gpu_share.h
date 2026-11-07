#pragma once

namespace holevr::gpu_share {

// Logs, every 10 s, how the graphics card's engines were shared between
// programs (Windows' own "GPU Engine" counters, the ones Task Manager shows):
// the game, SteamVR's compositor, the Steam Link stream, recording apps and
// anything else. Our GPU timer counts time other programs take in the middle
// of a game frame as the game's, so this says how much of the measured GPU
// frame is really the game. Runs on its own thread; gpu_share=0 turns it off.
void Start();

}  // namespace holevr::gpu_share
