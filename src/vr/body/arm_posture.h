#pragma once
// Body-relative elbow selection. Three tracked points do not measure elbows;
// this is an anatomical preference with explicit bounds, not joint tracking.
#include "vr/body/arm_pose.h"

namespace holevr::vr::arm_posture {

struct Input {
    arm_pose::Reference reference;
    FVector shoulder, wrist;
    FQuat hand_rotation;
    double upper_cm = 0, forearm_cm = 0, max_stretch_cm = 0;
    FVector outward, forward{1, 0, 0}, down{0, 0, -1};
    FVector forearm_axis_hand, palm_axis_hand;
    // Optional body-relative style preference. Do not derive this from a
    // free hand's orientation: a controller does not observe the elbow.
    FVector preferred_pole;
    // Optional actual body midline, e.g. the tracked head position. Only its
    // lateral coordinate is used; a head point is not a torso collision mesh.
    FVector torso_center;
    bool torso_center_valid = false;
    bool hand_frame_valid = true;
    // Explicit pose preferences for authored/recorded grips only. Ordinary
    // gameplay permits wrist bend and pronation; neither chooses the elbow.
    bool straight_wrist = false, wrist_relief = false;
    double dt = 0;
};

struct State {
    bool valid = false;
    FVector axis{1, 0, 0}, pole{0, 0, -1};
    // Hysteresis keeps a previously selected plane through extension noise.
    bool extension_locked = false;
    // Returning from an unobservable plane must not snap to a new domain.
    bool extension_recovering = false;
};

struct Result {
    bool valid = false;
    ik::TwoBoneResult solved;
    arm_pose::Rotations rotations;
    FVector pole, preferred_pole;
    // Radians, measured before skin-bone roll distribution. raw_roll is the
    // principal IK-frame roll, not a measured anatomical rotation.
    double raw_roll = 0, flex = 0, deviation = 0, swivel = 0, cost = 0;
    double torso_clearance_cm = 0;
    unsigned candidates = 0;
    bool constrained = false;
};

// One state per arm. Reset with state={} on tracking loss/recovery, origin,
// character or calibration changes. Invalid input clears state and returns
// invalid; the caller can preserve native animation rather than write NaNs.
// Candidate search uses one connected admissible interval. An established
// extension plane is transported unchanged, then returns to that interval at
// a bounded rate on retraction; arbitrary incoming states get no exemption.
Result Solve(const Input& input, State* state);

} // namespace holevr::vr::arm_posture
