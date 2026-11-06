#pragma once
// Conditional functional elbow fit: wrist_i = elbow + forearm * direction_i.
// The elbow must stay approximately still and the supplied forearm direction
// must be valid throughout the bend (e.g. a learned frame with a neutral wrist).
// Controllers do not directly observe an anatomical elbow. No shoulder, total
// arm length, assumed bend angle or prior segment proportion enters this fit.
#include <vector>
#include "vr/math.h"

namespace holevr::vr::elbow_calibration {
struct Observation { FVector wrist, forearm_direction; };
struct Result {
    bool valid = false;
    FVector elbow;
    double forearm_cm = 0;
    double rms_cm = 0;
    double span_degrees = 0;
    // Numerical model sensitivity: residual/noise floor divided by direction
    // excitation. This is not an anatomical confidence interval.
    double uncertainty_cm = 0;
    unsigned samples = 0, inliers = 0;
};
// Bounded to 512 samples. Robust isolated-outlier rejection requires >=85%
// retained observations AND angular-density weight. Repeated endpoint samples
// cannot create excitation; at least a quarter of the accepted observations
// must be outside their densest stationary cluster. Failure returns !valid.
Result Fit(const std::vector<Observation>& observations);
} // namespace holevr::vr::elbow_calibration
