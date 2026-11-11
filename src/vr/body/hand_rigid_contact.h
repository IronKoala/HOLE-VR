#pragma once
// Pure whole-hand contact projection. Fresh witnesses are fixed for this
// solve: moving tangentially beyond a finite patch never counts as clearance.
// The resulting translation still needs a world sweep before rendering.
#include "vr/body/ik_math.h"

namespace holevr::vr::body::hand_rigid_contact {
struct Plane { FVector point, normal, sample; double radius = 0; };
struct Reach {
    FVector wrist, shoulder;
    double maximum_cm = 0; // <=0 disables the optional IK reach ball
};
struct Settings {
    double skin_cm = 0.03, tolerance_cm = 0.015, maximum_translation_cm = 25;
    int iterations = 24;
};
struct Result {
    bool valid = false, feasible = false;
    FVector translation;
    double residual_cm = 0;
    int planes = 0, iterations = 0;
};
// Projects zero translation onto all contact halfspaces and optional reach
// ball. Infeasible/capped results are explicit; never render them as clear.
Result Resolve(const Plane* planes, int count, const Settings& settings = {}, const Reach& reach = {});
}
