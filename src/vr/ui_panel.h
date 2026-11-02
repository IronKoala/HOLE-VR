#pragma once

// Menu detection and menu buttons. A game menu is open while the game shows
// its mouse cursor; MenuOpen() tells the controls to stop firing. The menu
// button on either controller opens/closes the pause menu, B/Y goes back.
// The menus themselves are shown on the forearm by vr/world_menu; this module
// never shows a copy of the screen.
namespace holevr::vr::ui_panel {
// Before the VR session starts (registers with OpenXR).
void Init();
// From the background loop about once a second: finds the player controller.
void Maintain();
// True while a game menu is open, so gameplay input (firing) can stand down.
bool MenuOpen();
}  // namespace holevr::vr::ui_panel
