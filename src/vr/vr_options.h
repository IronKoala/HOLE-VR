#pragma once
// The VR tab in the game's Options menu. When Options comes up on the
// forearm, a "VR" button is added after the game's own tab buttons (Game,
// UI, Graphics, Audio, Controls, Keymap, Gamepad) and a page of rows, one per
// VR setting (vr/vr_menu_model). Both are made from the game's own button
// widgets, so they look and click like the rest of the menu. Clicking a row
// picks its next choice, saves it in hole-vr.ini at once and applies it.
#include <string>

#include "ue/objects.h"

namespace holevr::vr::vr_options {

// After game::Init() (looks up the engine classes and functions it needs).
void InitGame();
// From world_menu, on the game thread, once a menu is on the forearm.
void OnMenuHosted(ue::UObject menu, const std::string& class_name);
// From world_menu's frame pump (keeps running while paused).
void Tick();

}  // namespace holevr::vr::vr_options
