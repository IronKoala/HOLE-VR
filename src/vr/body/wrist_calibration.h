#pragma once
// A single controller-local anatomical hand basis, measured from explicitly
// neutral forward/T holds. This does not measure a wrist joint centre or an
// elbow. Positions retain the same estimated offset used by the arm fit.
#include <vector>
#include "vr/math.h"

namespace holevr::vr::wrist_calibration {

enum class Stage { Overhead, Forward, Out };
struct Observation {
    Stage stage = Stage::Forward;
    FQuat raw_grip_rotation;
    FVector forearm_direction, palm_direction;
    bool palm_known = false;
};
struct Calibration {
    bool measured = false;
    FVector forearm_axis_grip, palm_axis_grip;
    FVector wrist_offset_cm; // frozen controller-local physical estimate; not newly measured
    double forearm_rms_deg = 0, palm_rms_deg = 0, max_error_deg = 0;
};
enum class Reason { None, InvalidSample, NotEnoughSamples, InconsistentForearm, InconsistentPalm, InvalidOffset };
struct Result {
    Calibration calibration;
    Reason reason = Reason::None;
};
// Each forward/out hold needs at least three valid observations with a known
// palm direction. Both stages have equal weight regardless of sample density.
// The requested down/inward palm vector defines roll after projection onto
// the forearm-normal plane. Isolated angular/nonorthogonal spikes may be
// excluded only if at least 85% and three paired observations remain in EACH
// stage. Quality metrics describe those accepted samples. Coherently
// inconsistent stages still fail; one stage cannot be discarded or drowned
// out by the other. Legacy overhead observations are excluded from this solve.
Result Fit(const std::vector<Observation>& observations, const FVector& frozen_wrist_offset_cm);
bool Validate(const Calibration& calibration);

struct Mapping {
    FVector controller_world;
    FQuat grip_world;
    double tracking_scale = 1;
    FVector forearm_axis_hand, palm_axis_hand; // avatar reference directions in hand-bone space
    // Caller supplies current minus captured manual position (physical cm)
    // and Qmanual_current * inverse(Qmanual_captured), in raw-grip space.
    // Manual rotation affects orientation only, preserving calibrated wrist
    // position. Changing the controller-local position intentionally moves it.
    FVector position_delta_cm;
    FQuat rotation_delta_grip;
};
// Uses RAW grip pose, never an already corrected/snapped hand pose. Physical
// lengths are scaled once. Mapping is independent of avatar hand-bone axes;
// legacy profiles with measured=false must keep their old caller path.
// Failure leaves both outputs unchanged.
bool Map(const Calibration& calibration, const Mapping& input, FVector* wrist_world, FQuat* hand_world);

} // namespace holevr::vr::wrist_calibration
