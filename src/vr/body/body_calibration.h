#pragma once
// Three tracked points cannot observe shoulders or elbows directly. This
// estimates effective reach from comfortable forward/side poses, then checks
// an optional segment split against a single elbow bend. A separate final
// reach validates the candidate. Motion and model uncertainty are explicit;
// this is not anatomical joint tracking. No engine access or filesystem writes.

#include <cstdint>
#include <string>
#include <vector>

#include "vr/math.h"
#include "vr/body/wrist_calibration.h"

namespace holevr::vr::body_calibration {

enum class Phase { MeasureHeight=0, ArmsForward=3, ArmsOut=4, BendElbows=5, Complete=6, Failed=7, VerifyForward=8 };
enum class HeightMode { Fixed, Real, Seated };
enum class HeightPosture { Unspecified, Standing, Seated };
// A floor coordinate establishes position, not the user's physical posture.
// Fixed character height works while sitting or standing, so it cannot infer
// a height posture. A caller may explicitly choose one for calibration.
HeightPosture DefaultHeightPosture(HeightMode mode);
enum class Reason {
    None, TrackingLost, InvalidSample, TimeWentBackwards, OriginChanged,
    HeadMoved, TimedOut, NotEnoughArc, UnstablePose, BadArcFit,
    ArmsNotLevel, BentPoseMismatch, InvalidProfile, LookForward,
    HandsTooLow, StageTimedOut, HeightUnavailable, HeightModeChanged, HeightUnspecified,
    MoreBendMotion, UncertainBend, WristPoseMismatch, VerificationMismatch,
    ArmDirectionMismatch
};

struct Sample {
    double seconds = 0;
    FVector head;
    FQuat head_rotation;
    // Anatomical wrist targets, derived by the caller from the tracked
    // controllers using its existing hand mapping. All positions use the
    // SAME stable world frame, in cm. Never pass snapped/grabbed IK targets.
    FVector wrist[2];  // left, right
    FQuat grip_rotation[2]; // RAW tracked orientation, before visual hand correction
    bool grip_orientation_valid = false;
    FVector wrist_offset_grip_cm[2]; // frozen base wrist offset in raw grip-local physical cm
    FVector hand_position_setting_game_cm[2];
    FQuat hand_rotation_setting[2];
    bool valid = false;  // all three tracked poses valid for this sample
    bool head_valid = false;  // height step may proceed with untracked hands
    bool floor_known = false;  // raw head.Z is floor-relative physical cm
    FVector prior_shoulder[2] = {{-5,-20,-25},{-5,20,-25}};
    double prior_reach_cm[2] = {60,60}; // explicit fallback model, not anatomy measured here
    double prior_forearm_fraction[2] = {.47,.47}; // caller-supplied prior, not a measurement
    bool skip_arms = false;  // queued until height step is finished
    bool confirm_pose = false; // fresh user edge: this pose is comfortable and ready to record
    // Runtime supplies the continuous dual-trigger gate. Releasing either
    // trigger cancels only the active window. True preserves historical
    // edge-only replay callers; live input must set this explicitly.
    bool capture_held = true;
    HeightMode height_mode = HeightMode::Fixed;
    HeightPosture height_posture = HeightPosture::Unspecified;
    std::uint64_t origin_generation = 0;  // increment on recenter/world remap
};

struct Fit {
    double radial_rms_cm = 0;
    double plane_rms_cm = 0;
    double arc_degrees = 0;
    double bent_error_cm = 0;
    unsigned inliers = 0;
    unsigned samples = 0;
    double bend_span_degrees = 0, bend_length_spread_cm = 0;
    unsigned bend_samples = 0, bend_inliers = 0;
};
struct Arm {
    // Effective offset from the head, in the session's frozen yaw frame:
    // +X forward, +Y right, +Z up. This is an inferred effective pivot.
    FVector shoulder;
    double upper_cm = 0;
    double forearm_cm = 0;
    Fit fit;
    wrist_calibration::Calibration wrist;
    FVector hand_position_setting_game_cm;
    FQuat hand_rotation_setting;
    bool shoulder_measured = false; // true only when all three pivot directions are supported
    unsigned shoulder_observed_dimensions = 0; // null directions retain the supplied prior
    bool segments_measured = false; // false: upper and/or forearm remains conditional or estimated
    bool forearm_measured = false; // independent elbow-motion fit; upper may still be conditional
    double verification_error_cm = 0;
};
struct Profile {
    static constexpr unsigned kVersion = 6;
    unsigned version = kVersion;
    // Independently measured floor-to-eye baselines; zero means unknown.
    // Neither seated nor unclassified height may set standing body geometry.
    double standing_eye_cm = 0;
    double seated_eye_cm = 0;
    // v1/v2 did not record physical posture. Preserve that old value for
    // provenance without silently trusting its former "standing" label.
    double legacy_eye_cm = 0;
    // False means keep existing arm settings. No length/pivot is inferred.
    bool arms_measured = true;
    Arm arm[2];
};

struct Settings {
    double preparation_seconds = 2.0;
    double hold_seconds = 2.0;
    double hold_rms_cm = 2.5;
    double hold_drift_cm = 3.5;
    double orientation_rms_deg = 12.0;
    double max_head_rotation_deg = 40.0;
    double observation_gap_seconds = 1.0;
    double timeout_seconds = 600.0;
    double stage_timeout_seconds = 120.0;
    double bend_min_span_deg = 20.0;
};

struct Status {
    Phase phase = Phase::MeasureHeight;
    Reason reason = Reason::None;
    double hold_fraction = 0;
    double elapsed_seconds = 0;
    double stage_elapsed_seconds = 0;
    double preparation_remaining_seconds = 0;
    bool baseline_locked = false;
    bool height_measured = false;
    HeightPosture height_posture = HeightPosture::Unspecified;
    double measured_eye_cm = 0;  // valid only when height_measured
    double standing_eye_cm = 0;  // compatibility: nonzero only for measured standing posture
    bool arms_measured = false;
    unsigned attempt = 1;  // increments when partial measurements are discarded
    // Difference from the compared head reference. Retained on the status
    // that restarts an attempt, so callers can explain/log the actual cause.
    double head_motion_cm = 0;
    double head_rotation_deg = 0;
    unsigned arc_samples = 0;
    unsigned bend_samples = 0;
    unsigned bend_attempt = 1;
    double bend_span_degrees = 0, bend_fit_error_cm = 0;
    // Legacy readiness diagnostics remain for logs. User confirmation controls
    // capture; no inferred elbow pose or elapsed timer authorizes a stage.
    bool bend_ready = false, bend_pose_bent = false;
    bool pose_ready = false;
    bool pose_confirmed = false; // user declaration, never inferred from assumed joint positions
    bool capture_accepted = false; // one Update only; an actually accepted capture, never a skip/retry
    bool bend_degraded = false, reach_degraded = false;
    int rejected_side = -1;
    Reason bend_reason[2] = {Reason::None, Reason::None};
    double pose_rms_cm = 0, pose_drift_cm = 0, orientation_rms_deg = 0;
    double verification_error_cm = 0;
};

class Session {
public:
    explicit Session(const Settings& settings = Settings{});
    void Reset();
    Status Update(const Sample& sample);
    const Status& GetStatus() const { return status_; }
    // No partial profile escapes. The caller only commits a complete result
    // and must retain its previous calibration on cancel/failure.
    const Profile* GetProfile() const { return status_.phase == Phase::Complete ? &profile_ : nullptr; }

private:
    struct HeldSample { double seconds; FVector wrist[2]; FQuat grip[2]; };
    Settings settings_;
    Status status_;
    Profile profile_;
    bool started_ = false, have_baseline_ = false;
    double started_at_ = 0, last_time_ = 0;
    double preparation_until_ = 0, stage_started_at_ = 0;
    bool tracking_interrupted_ = false;
    bool skip_arms_ = false, height_done_ = false, height_floor_known_ = false;
    std::uint64_t origin_ = 0;
    HeightMode height_mode_ = HeightMode::Fixed;
    HeightPosture height_posture_ = HeightPosture::Unspecified;
    FVector head_;
    FQuat head_rotation_, world_to_reference_;
    FVector forward_[2], straight_[2];
    double reach_[2] = {}, prior_fraction_[2] = {.47,.47};
    std::vector<HeldSample> held_;
    std::vector<HeldSample> straight_hold_[2], straight_motion_, bent_;
    FVector prior_shoulder_[2] = {{-5,-20,-25},{-5,20,-25}};
    double prior_reach_[2] = {60,60};
    FVector frozen_offset_[2], frozen_position_setting_[2];
    FQuat frozen_rotation_setting_[2];
    void Fail(Reason reason);
    void Enter(Phase phase, double seconds, double preparation);
    void CompleteHeightOnly();
    bool Hold(const HeldSample& sample, FVector mean[2]);
    bool FitArms();
    void FitBent();
    bool Verify(const FVector mean[2]);
};

struct BendObservation { FVector wrist; FQuat grip_rotation; };
struct SegmentFit {
    double upper_cm = 0, forearm_cm = 0, rms_cm = 0, span_degrees = 0, length_spread_cm = 0;
    unsigned samples = 0, inliers = 0;
};
// Fit many informative bends, not an ideal endpoint. The controller's learned
// longitudinal direction assumes a neutral wrist. Failure leaves out intact.
Reason FitSegments(const FVector& shoulder, double total_reach_cm,
                   const wrist_calibration::Calibration& wrist,
                   const std::vector<BendObservation>& observations,
                   double minimum_span_degrees, SegmentFit* out);

const char* Prompt(Phase phase, HeightPosture posture = HeightPosture::Unspecified);
const char* ReasonText(Reason reason);
bool Validate(const Profile& profile);
std::string Serialize(const Profile& profile);
// Strict, bounded, versioned format. Failure does not change *out.
bool Parse(const std::string& text, Profile* out);
// Combine a new result with an optional previous fit. Zero height fields in
// result preserve prior baselines; height-only results preserve prior arms.
// The caller decides whether a no-op result should be saved at all. Invalid
// input leaves *out unchanged. previous==nullptr starts a new profile.
bool MergeProfile(const Profile* previous, const Profile& result, Profile* out);

// The forearm's share of shoulder-to-wrist reach that a human arm can have.
// Adult joint-centre proportions run about .41-.47; the range is widened for
// the controller-based wrist point and the avatar's .47. An elbow fit outside
// it means the elbow moved during the bend (it read one owner's forearm as
// 20.8 cm against 29.4 on the other arm), so it is not used.
constexpr double kMinForearmShare = .40, kMaxForearmShare = .53;
// The split that replaces an implausible one, on the same total reach.
constexpr double kDefaultForearmShare = .47;
bool PlausibleSplit(double upper_cm, double forearm_cm);
// The upper/forearm lengths the body uses for this arm: the saved ones, or,
// when their split is not plausible, kDefaultForearmShare of the same reach.
// Returns false when it had to replace the saved split.
bool UsableSegments(const Arm& arm, double* upper_cm, double* forearm_cm);

// Convert the persisted physical fit to this frame's world targets. The
// tracking scale applies once; avatar mesh scale must not resize the user.
// body_yaw is the body's world yaw, independent of mesh bone orientation.
// Upper and forearm come from UsableSegments.
// Failure leaves every output unchanged.
bool MapArmTarget(const Profile& profile, int side, const FVector& head_world,
                  const FQuat& body_yaw, double tracking_scale,
                  FVector* shoulder_world, double* upper_world, double* forearm_world);

// FreeHand supplies an already rotated game-cm wrist-minus-controller
// vector. Convert only that displacement to real cm, preserving the raw
// tracking-space origin and the direction of the hand offset.
bool MapWristTarget(const FVector& raw_controller_cm, const FVector& wrist_game_delta_cm,
                    double tracking_scale, FVector* wrist_real_cm);

}  // namespace holevr::vr::body_calibration
