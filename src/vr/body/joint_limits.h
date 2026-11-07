#pragma once
// Human joint ranges for posed fingers (owner, 2026-11-04: "humans cant bend
// our last 2 finger joints side to side").
//
// A finger joint's turn, relative to the skeleton's reference pose and in the
// joint's own frame (the grip record's pose_* format), is split into three
// parts about the joint's anatomical axes:
//   - flex: the curl toward the palm (+) or back (-), about the flex axis;
//   - side: the sideways bend (abduction), how far the bone leans out of
//     its curl plane;
//   - twist: the roll about the bone itself.
// Each joint gets a range for each part, from hand anatomy:
//   - finger middle and tip joints (PIP, DIP) are hinges: they only curl;
//   - finger knuckles (MCP) curl, lean sideways a little (less when curled:
//     the side ligaments tighten) and barely roll;
//   - the thumb's base (CMC) is a saddle: it curls, swings sideways and
//     rolls toward the fingers (opposition); its middle joint (MCP) is
//     mostly a hinge with a little side play; its tip (IP) is a hinge.
// Curl ranges are anatomical (0 = a straight finger); the skeleton's
// reference pose is already a little curled, so each range is shifted by the
// bend measured in the reference pose.
//
// Fit() makes an existing pose human while keeping its shape: side bends
// taken out of a hinge are made up, where the knuckle can, by the knuckle,
// so the fingertip stays near where it was posed.
//
// Pure math, unit tested in tests/test_grip.cpp.

#include "vr/body/finger_solve.h"

namespace holevr::vr::joint_limits {

// One joint's human range, degrees.
struct Range {
    double flex_min = 0, flex_max = 0;  // curl, 0 = straight, + = toward the palm
    double side = 0;                    // sideways either way; 0 = a hinge
    double side_curled = 1.0;           // share of `side` left at full curl
    double twist = 0;                   // roll about the bone either way
    bool from_reference = false;        // curl measured from the reference pose (thumb base)
};
// finger: 0 thumb, 1 index, 2 middle, 3 ring, 4 pinky (hand_pose::Finger).
// Joints: 0 base (knuckle; thumb: CMC), 1 middle, 2 tip.
Range Human(int finger, int joint);

// The joint's axes in its own reference frame.
struct Axes {
    FVector bone;    // along the bone, toward the next joint
    FVector flex;    // flex axis (+ curls)
    FVector toward;  // where a + curl takes the bone (palm side)
};
Axes JointAxes(const fingers::Chain& c, int j);

// A joint turn split into its parts, radians.
struct Turn {
    double flex = 0, side = 0, twist = 0;
};
Turn Decompose(const Axes& a, const FQuat& q);
FQuat Compose(const Axes& a, const Turn& t);

// Bend of each joint in the reference pose, radians (+ = curled). The base
// joint is measured from the wrist->knuckle line, so the thumb's base reads
// 0 (its range is about the reference pose). The tip joint reads 0 too: the
// chain's fingertip carries straight on from the middle bone, so there is
// no bend to measure there. Clamped to a sane span.
void ReferenceBends(const fingers::Chain& c, bool thumb, double out[3]);

// A joint's range in record terms (about the reference pose), radians.
struct Limits {
    double flex_min = 0, flex_max = 0;
    double side = 0, side_curled = 1.0, twist = 0;
    double ref_bend = 0;     // reference pose bend (anatomical curl at flex 0)
    double anat_max = 1.0;   // anatomical full curl, for the side narrowing
    bool hinge() const { return side <= 0 && twist <= 0; }
    double SideAt(double flex) const;  // side range at this curl
};
Limits Relative(const Range& r, double ref_bend);

// Clamps a turn into the range. *at_limit: the curl or (for joints with
// side play) the side bend ran into an end of its range.
Turn ClampTurn(const Limits& l, const Turn& t, bool* at_limit = nullptr);
// The joint's rotation (record format) limited to the range. A rotation
// already in range comes back unchanged.
FQuat Clamp(const Axes& a, const Limits& l, const FQuat& q, bool* at_limit = nullptr);
bool Within(const Axes& a, const Limits& l, const FQuat& q, double tolerance_rad = 0.005);

struct FitReport {
    bool changed = false;
    double moved_cm = 0;     // furthest a joint or the fingertip moved
    double side_deg[3] = {}, twist_deg[3] = {};  // sideways bend / roll taken out of each joint
    double flex_deg[3] = {};                     // curl past the range taken out
};
// Makes a posed finger (record format, three joints) human, as close to its
// shape as the ranges allow. A finger already in range is left untouched.
FitReport Fit(const fingers::Chain& c, const Axes ax[3], const Limits lim[3], FQuat q[3]);

}  // namespace holevr::vr::joint_limits
