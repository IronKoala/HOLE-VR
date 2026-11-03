#pragma once
#include <cstdint>

// Menus as panels in the world.
//
// HOLE's menus (title, pause, options, weapon customisation, results...) are
// UMG widgets the game adds to the flat screen. While one is open we take it
// off the screen and put it into a WidgetComponent: a real panel in the 3D
// world, rendered by the engine in both eyes like any object, with nothing of
// the flat screen behind it. Either controller points at it: the panel's own
// interaction component turns the laser into hover and clicks, the thumbstick
// scrolls. Sub-menus get their own panel just in front of the one they came
// from.
//
// When the game closes a menu we let go of it; when the menu disappears from
// view entirely (cursor gone) every panel is removed. There is no flat-screen
// fallback: a menu is on the forearm or nowhere.
namespace holevr::vr::world_menu {

// Before the VR session starts (registers the laser layer with OpenXR).
void Init();
// After game::Init(): hooks the game-thread events that drive the panels.
void InitGame();
// Background thread, a few times a second: finds the player controller
// whose tick drives the menus (it keeps ticking while the game is paused).
void Scan();
// True while a game menu is on the forearm (the game is in a menu).
bool MenuUp();
// True while the pause menu is the menu on top (any thread).
bool PauseMenuOnTop();
// Any thread: close the pause menu on the next game frame (the menu button
// toggles it).
void RequestClosePause();
// True while at least one menu is on a world panel.
bool Active();
// True when world panels are switched on in the settings.
bool Enabled();
// Game thread: the actor holding the menu and HUD panels (0 if there is
// none now). Its panels block the game's visibility traces, so traces that
// stand for the player's own body (hands stopping at walls, the head-in-wall
// fade) ignore it: looking at the wrist put the panels between the eyes and
// the left hand, which stopped the hand up to 22 cm short (slide pinch
// thread, run 2026-11-03 10:47).
uintptr_t PanelHolder();

}  // namespace holevr::vr::world_menu
