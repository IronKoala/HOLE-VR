#pragma once
// Same-hand separation for fallback capsules and fitted directional surfaces.
#include "vr/body/finger_contact.h"

namespace holevr::vr::finger_self {
struct Result {
    double residual_cm=0, world_residual_cm=0;
    int active_pairs=0, passes=0;
    bool restricted=false;
    bool feasible=true; // false is an invalid rig/initial pose, never "clear"
    bool yielded=false;
};
double Depth(const fingers::Chain chains[5],const fingers::Angles poses[5],
             const skel::Xf& hand,double radius_cm,int* active_pairs=nullptr);
// Run on prepared motion and again after world arrest changes any joint. Certified
// separation is hard; bounded self-only yielding helps release interlocks.
// Any resulting world debt MUST be resolved by checked rigid-root feedback
// before rendering/Commit. All passes share the original angular budget.
Result Resolve(const fingers::Chain chains[5],fingers::Angles poses[5],
               finger_contact::State states[5],const skel::Xf& hand,
               const finger_contact::Bounds bounds[5],
               const finger_contact::Contacts world[5],double radius_cm,double dt);
} // namespace holevr::vr::finger_self
