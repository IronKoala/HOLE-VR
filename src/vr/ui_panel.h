#pragma once

// Floating menu screen. While one of the game's menus is open (title, pause,
// options, weapon customisation, results...), the game window is shown on a
// flat panel in front of the player as an OpenXR quad layer, and either
// controller works as a laser pointer: aim at the panel, trigger to click,
// thumbstick up/down to scroll, B/Y to go back. The menu button on either
// controller opens and closes the pause menu.
//
// By default menus are moved onto their own panels in the world instead
// (vr/world_menu); this module then only detects menus and handles the
// menu/back buttons. The flat copy of the whole window is the fallback
// (menu_mode=window, or when no menu could be moved).
namespace holevr::vr::ui_panel {
// Before the VR session starts (registers with OpenXR).
void Init();
// From the background loop about once a second: finds the player controller.
void Maintain();
// True while a game menu is open, so gameplay input (firing) can stand down.
bool MenuOpen();
}  // namespace holevr::vr::ui_panel
