#pragma once

// Menu detection and menu buttons. A game menu is open while the game shows
// its mouse cursor; MenuOpen() tells the controls to stop firing. The menu
// button on either controller opens/closes the pause menu, B/Y goes back.
// The menus themselves are shown on the forearm by vr/world_menu; this module
// never shows a copy of the screen. The cursor is read on the game thread;
// the present thread (buttons) only reads flags, never game memory.
namespace holevr::vr::ui_panel {
// Before the VR session starts (registers with OpenXR).
void Init();
// Once the game hooks are up: reads the cursor flag in the player's tick.
void InitGame();
// From the background loop about once a second: finds the cursor flag's
// place in PlayerController (once).
void Maintain();
// True while a game menu is open, so gameplay input (firing) can stand down.
bool MenuOpen();
// Game-thread watch contact queues a short-lived open-only request. The
// present thread rechecks menu/calibration/editor state before sending it.
bool RequestPauseFromWatch();
}  // namespace holevr::vr::ui_panel
