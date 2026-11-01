#pragma once

// Floating menu screen. While one of the game's menus is open (title, pause,
// options, weapon customisation, results...), the game window is shown on a
// flat panel in front of the player as an OpenXR quad layer, and either
// controller works as a laser pointer: aim at the panel, trigger to click,
// thumbstick up/down to scroll, B/Y to go back. The menu button on either
// controller opens and closes the pause menu.
//
// The panel shows the whole window, so the game is visible behind the menu,
// like a cinema screen. HUD elements during play are handled separately
// (wrist HUD).
namespace holevr::vr::ui_panel {
// Before the VR session starts (registers with OpenXR).
void Init();
// From the background loop about once a second: finds the player controller.
void Maintain();
// True while a game menu is open, so gameplay input (firing) can stand down.
bool MenuOpen();
}  // namespace holevr::vr::ui_panel
