#pragma once
// Body-relative elbow selection. Three tracked points do not measure elbows;
// this is an anatomical preference with explicit bounds, not joint tracking.
#include "vr/body/arm_pose.h"

#include <limits>

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
    // Human joint ranges (owner 2026-11-07: "the only time humans elbows go
    // above our head is when we reach straight up"; every arm joint needs a
    // twist and bend limit). Shoulder: the elbow stays at or below shoulder
    // height unless the hand is raised or reaches back over the shoulder.
    // Forearm: about 85 deg of turn each way from thumb-up; the shoulder
    // takes shoulder_twist_share of that turn and everything past it. Wrist:
    // 70 deg back, 80 deg toward the palm, 20 deg toward the thumb, 35 deg
    // toward the little finger, for free hands and gun grips alike. Forearm
    // and wrist ranges need the hand frame and fade with the other hand terms
    // at full extension. Where the arm cannot reach below a height limit,
    // the limit starts a little above the lowest reachable elbow.
    bool joint_limits = true;
    // The hand holds a gun (its grip or a support hold; needs joint_limits):
    // the elbow hangs about 45 deg down when the hand is at shoulder height,
    // rising with the hand, and stays behind the hand rather than ahead of it.
    bool gun_hold = false;
    // The hand is a support hand on a gun (needs joint_limits). Held well out
    // in front of the chest, its elbow may turn in under the gun, past the
    // midline guard, to keep the forearm's turn in range.
    bool support_hold = false;
    double shoulder_twist_share = .15;
    // Thumb side across the knuckles, hand space (index minus little-finger
    // knuckle). Optional: without it both wrist side bends use 25 deg.
    FVector radial_axis_hand;
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
    // Joint-range diagnostics. forearm_rotation: 0 = palm toward the biceps
    // side of the bend (fully supinated), pi/2 = thumb up, pi = palm away
    // (fully pronated); NaN when the palm cannot be measured. elbow_*_cm are
    // heights above the shoulder along the body's up.
    double forearm_rotation = std::numeric_limits<double>::quiet_NaN();
    double elbow_height_cm = 0, elbow_cap_cm = 0, elbow_over_cm = 0;
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
