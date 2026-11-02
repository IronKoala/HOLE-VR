#pragma once

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
// view entirely (cursor gone) every panel is removed. If no menu could be
// moved, the old flat-screen panel (vr/ui_panel) takes over.
namespace holevr::vr::world_menu {

// Before the VR session starts (registers the laser layer with OpenXR).
void Init();
// Background thread, a few times a second: finds the game's menu widgets.
void Scan();
// True while at least one menu is on a world panel.
bool Active();
// True when world panels are switched on in the settings.
bool Enabled();

}  // namespace holevr::vr::world_menu
