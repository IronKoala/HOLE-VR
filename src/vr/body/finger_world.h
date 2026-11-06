#pragma once

#include "ue/objects.h"
#include "vr/body/finger_world_query.h"

namespace holevr::vr::finger_world {
using Stats = finger_world_query::Stats;
void LoadSettings();
bool Enabled();
void Reset(int side = -1);

// Game thread, after hand_walls::Apply has refreshed its ignore list. Query
// requested geometry, then pass the returned per-digit patches to the pure
// contact solver. Does not change the request, game objects or skeletal mesh.
Stats Update(ue::UObject player, int side, const fingers::Chain chains[5],
             const fingers::Angles proposed[5], const skel::Xf& hand,
             double radius_cm, double dt, finger_contact::Contacts output[5]);
// Call after the final displayed pose is selected; temporal sweeps must use
// actual resolved geometry rather than an unreachable previous request.
void Commit(int side, const fingers::Chain chains[5], const fingers::Angles resolved[5],
            const skel::Xf& hand, double radius_cm = 0.5);
}  // namespace holevr::vr::finger_world
