#pragma once
// Thumb-specific generated targets. These are tunable interaction curves,
// not measured human constants. Authored grips should retain their own poses.
#include "vr/body/finger_contact.h"

namespace holevr::vr::thumb_pose {

struct Input {
    double grip = 0;
    double trigger = 0;
    bool thumb_touch = false;
};

// palm and index_base are measured in hand space from the reference rig.
// Opposition combines base side motion with axial rotation chosen from the
// actual rig geometry, so the same code works on mirrored/rolled bone frames.
fingers::Angles Target(const fingers::Chain& chain, const FVector& palm,
                       const FVector& index_base, const Input& input,
                       const finger_contact::Bounds& bounds);

// Reference-derived pad normal at the thumb tip, in hand space.
FVector PadDirection(const fingers::Chain& chain, const fingers::Angles& pose,
                     const FVector& palm);

}  // namespace holevr::vr::thumb_pose
