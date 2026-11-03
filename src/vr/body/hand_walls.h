#pragma once
// Empty hands stop at walls (owner's feature list, 2026-11-03, item 12:
// "physics hands that stop at walls").
//
// The VR body's hand is drawn where the controller is, unless something is
// in the way: a fist-sized ball is swept from the eyes to the hand, and where
// it first touches a wall, floor, door, table or person, the drawn hand stops
// against that surface. The controller itself is left alone, so grabbing,
// holsters, doors and menus still use the real hand. A hand that holds
// something follows that object instead (the gun keeps itself out of walls on
// its own: controls WallPushback).
//
// Settings: hand_wall_collision (1 = on), hand_wall_radius_cm (ball radius).
#include "ue/objects.h"
#include "vr/hands.h"

namespace holevr::vr::body::hand_walls {

using ue::UObject;

void LoadSettings();

// hs: this frame's hands as the body is about to draw them. An empty hand
// that would be inside something is moved back to the surface (grip, aim and
// wrist points move together; rotations are kept).
void Apply(UObject player, const hands::Pose& head, hands::HandState hs[2], const hands::HandGrab grab[2],
           double dt);

}  // namespace holevr::vr::body::hand_walls
