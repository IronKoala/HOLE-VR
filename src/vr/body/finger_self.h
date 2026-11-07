#pragma once
// Same-hand capsule separation, verified against current world contacts.
#include "vr/body/finger_contact.h"

namespace holevr::vr::finger_self {
struct Result {
    double residual_cm=0, world_residual_cm=0;
    int active_pairs=0, passes=0;
    bool restricted=false;
};
double Depth(const fingers::Chain chains[5],const fingers::Angles poses[5],
             const skel::Xf& hand,double radius_cm,int* active_pairs=nullptr);
// Run after world Solve calls and before rendering/Commit. Every additional
// solve shares the original frame's angular bound. Real capsule distances
// and current world constraints are rechecked before accepting a pose.
Result Resolve(const fingers::Chain chains[5],fingers::Angles poses[5],
               finger_contact::State states[5],const skel::Xf& hand,
               const finger_contact::Bounds bounds[5],
               const finger_contact::Contacts world[5],double radius_cm,double dt);
} // namespace holevr::vr::finger_self
