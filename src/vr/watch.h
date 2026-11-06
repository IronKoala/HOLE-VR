#pragma once
namespace holevr::vr::watch {
void InitGame();
// Game-thread controller/menu pump: keeps local time advancing while paused.
void MenuTick();
// The existing menu stack confirms a live results panel, including death.
void ResultsOpened();
} // namespace holevr::vr::watch
