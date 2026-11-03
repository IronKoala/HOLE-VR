#pragma once

// Logging-only study of how HOLE runs its menus. Changes nothing in the game.
//
// - Logs the game's own Blueprint events on its menu widgets (UIM_/UIP_/UIS_),
//   the game instance, the player controller and the UI pawn, so we see which
//   object opens the pause menu and in what order.
// - Snapshots every live menu widget when a menu opens/closes: its outer
//   chain (who created it), visibility, opacity and parent panel.
// - While a menu is open, counts which events still fire each second (the
//   game is paused then, so this finds a safe place to run our own code).
// - Asks the OpenXR thread for a picture of what the headset is shown.
//
// Off by default; menu_study=1 in hole-vr.ini turns it on.
namespace holevr::vr::menu_study {

// After game::Init(): hooks the widget and game-instance event paths.
void InitGame();
// Background thread, every ~16 ms. Memory reads only, no game calls.
void Poll();

}  // namespace holevr::vr::menu_study
