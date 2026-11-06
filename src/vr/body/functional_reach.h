#pragma once
// Comfortable straight-arm trajectories estimate an effective pivot; no
// shoulder or elbow is directly tracked. FitPositions learns only supported
// spatial directions and retains prior information in the remaining nullspace.
// The rotation-based legacy helpers below retain their separate contracts.
#include <vector>
#include "vr/math.h"

namespace holevr::vr::functional_reach {
struct Observation { FVector wrist; FQuat grip; };
using Group = std::vector<Observation>;
struct Result {
    bool valid = false;
    bool observed = false; // all three directions constrained by the selected model
    FVector shoulder;
    double reach = 0;
    double rms = 0; // largest accepted per-group positional RMS, cm
    double condition = 0; // smallest/largest excitation eigenvalue
    unsigned samples = 0, inliers = 0;
    unsigned observed_dimensions = 0; // FitPositions: solved spatial directions, 0..3
    double pivot_sensitivity_cm = 0; // FitPositions: residual amplification on solved axes only
};
// Nonempty groups have equal total weight, regardless of sampling density.
// Each needs >=3 samples; at most 15% isolated residuals may be excluded in
// EACH group. The accepted set must retain the observability and fit gates.
// Rank-deficient motion, or residuals amplified beyond a 5cm pivot sensitivity
// bound, fix shoulder to priorShoulder and estimate reach conditional on it
// (observed=false). This never claims a directly measured anatomical joint.
// Failed fits return valid=false; prior inputs must be finite/plausible.
Result Fit(const std::vector<Group>& groups, FVector priorShoulder, double priorReach);
// Conditional endpoint-only reach fallback. Keep the supplied shoulder
// exactly; controller orientation does not establish a new pivot here.
// Requires exactly two endpoint groups, >=3 and >=85% radial inliers per
// group, <=2cm radial RMS/4cm max residual, and their mean radii within
// min(3cm,5% reach). Groups have equal weight. The caller must still validate
// neutral-wrist/palm consistency independently. Always observed=false.
Result FitPriorReach(const std::vector<Group>& endpoints, FVector priorShoulder);
// Position-only sphere/circle fit. The first two groups are endpoint holds;
// further groups contain the comfortable straight-arm transition. Grip
// rotations are deliberately ignored. Equal stage weights and >=85% inliers
// per stage prevent a dense hold from erasing a rejected motion stage.
// Only sufficiently excited position directions update the supplied pivot.
// A planar arc keeps the prior's normal component; endpoints keep two null
// components. observed is true ONLY with three supported spatial directions.
// Endpoint-only fits cannot distinguish a shorter reach from a shifted pivot.
// Radial residuals <=2cm RMS /4cm max, endpoint hold noise, spatial eigenvalue
// floors and an 8cm residual-amplification guard bound the usable directions.
// Unobserved components retain prior uncertainty; this is not joint tracking.
Result FitPositions(const std::vector<Group>& groups, FVector priorShoulder, double priorReach);
} // namespace holevr::vr::functional_reach
