#pragma once
// The approach pose is separate from the saved, held-pistol grip.
#include "vr/body/finger_solve.h"
#include "vr/body/grip_record.h"

namespace holevr::vr::holster_pose {
// Straighten all three segments along a hand-space direction, preserving
// their lengths and the base joint. Also removes reference-pose joint bends.
fingers::Angles Extended(const fingers::Chain& chain, const FVector& direction);

struct Pose {
    fingers::Angles thumb, index;
};
// hand_in_gun is the saved wrist orientation in the semantic gun frame
// (+X barrel, +Y right, +Z sights). The thumb is raised above the grip;
// the index is fully extended along the outside of the barrel/holster.
Pose Approach(const fingers::Chain& thumb, const fingers::Chain& index,
              const FQuat& hand_in_gun);
// Clearance for the fitted shell on the gun's right side, in centimetres.
inline constexpr double kPalmOut = 1.8;
// Lift the extended finger from the lower rim onto the shell's side. This
// changes only the index MCP direction, preserving all segment lengths.
inline constexpr double kIndexInward = .04;
inline constexpr double kIndexRise = .25;
struct Grip {
    FVector hand_position;
    fingers::Angles finger[5];
};
// Called on grip setup/refresh, never searched every rendered frame. The
// three remaining fingers keep their saved contact targets on the pistol.
Grip Fit(const grip_record::Record& record, const fingers::Chain* chains,
         const FQuat& hand_in_gun, const FVector& hand_position, double radius);
}  // namespace holevr::vr::holster_pose
