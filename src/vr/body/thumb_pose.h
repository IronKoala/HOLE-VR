#pragma once
// Thumb-specific generated targets. These are tunable interaction curves,
// not measured human constants. Authored grips should retain their own poses.
#include "vr/body/controller_pose.h"

namespace holevr::vr::thumb_pose {

struct Input {
    double grip = 0;
    double trigger = 0;
    bool thumb_touch = false;
};

// palm and index_base are measured in hand space from the reference rig.
// The generated base stays near its rig-derived neutral saddle side/roll;
// face/stick-button input folds MCP/IP only. Grip/trigger retain the outside
// guide and anatomical closing flex. Mirrored/rolled frames use the same math.
fingers::Angles Target(const fingers::Chain& chain, const FVector& palm,
                       const FVector& index_base, const Input& input,
                       const controller_pose::Bounds& bounds);

// Reference-derived pad normal at the thumb tip, in hand space.
FVector PadDirection(const fingers::Chain& chain, const fingers::Angles& pose,
                     const FVector& palm);

}  // namespace holevr::vr::thumb_pose
