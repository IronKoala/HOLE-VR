#pragma once
// Arm bone frames measured once from the skeleton's reference pose. The
// runtime mapping never reads gun/aim animation rotations. All positions
// passed to one operation must be in the same coordinate space, in cm.
#include "vr/body/skeleton.h"

namespace holevr::vr::arm_pose {

struct Reference {
    bool valid = false;
    double upper_length = 0;
    double forearm_length = 0;
    FVector upper_axis;       // shoulder -> elbow, in upper-arm bone space
    FVector forearm_axis;     // elbow -> wrist, in forearm bone space
    FVector upper_normal;     // reference bend-plane normal in each bone
    FVector forearm_normal;
    FVector upper_scale{1, 1, 1};
    FVector forearm_scale{1, 1, 1};
    FQuat forearm_to_hand;    // includes any intervening twist/roll bones
};

// The transforms are COMPONENT-space reference transforms, not the three
// local bone transforms. preferred_bend is an anatomical fallback direction
// in that same space (e.g. the body's forward direction) for a straight rest
// arm. Bone axes and parent layouts may differ between left/right arms.
Reference MeasureReference(const skel::Xf& upper, const skel::Xf& forearm,
                           const skel::Xf& hand, const FVector& preferred_bend);

struct Rotations {
    bool valid = false;
    FQuat upper;
    FQuat forearm;
};

// Map measured bone frames to the final two-bone solution. Arms use the
// constrained posture solver's pole; legs can use their source animation's
// knee plane. pole must match the solution and supplies a stable frame at
// full extension, where the triangle itself no longer defines a plane.
Rotations Orient(const Reference& reference, const FVector& shoulder,
                 const ik::TwoBoneResult& solved, const FVector& pole);

// Match skinned segment length as well as its joint endpoints. Moving only
// the child joint leaves vertices weighted to its parent at the old length.
// FTransform supports diagonal local scale: require a reference bone whose
// longitudinal axis is cardinal (the avatar's arm bones use +/-X). Keep the
// two transverse world scales, so limb thickness is unchanged. Apply each
// segment independently and restore the hand's baseline absolute world scale
// after scaling its parents. Failure leaves *out untouched.
bool MatchSegmentScale(const FVector& reference_axis, const FVector& reference_scale,
                       double reference_length, double solved_length,
                       const FVector& baseline_world_scale, FVector* out);

// Signed wrist twist in radians before distribution to forearm/twist bones.
// Uses the reference forearm-to-hand relation, not hand's immediate local
// rotation (its parent can be an intervening twist bone).
double WristTwist(const Reference& reference, const FQuat& forearm_world,
                  const FQuat& hand_world);

struct WristPole {
    bool valid = false;
    FVector pole;
    double strength = 0; // 0: desired forearm parallel to shoulder->wrist; plane is unobservable
};
// Elbow-plane direction minimizing the angle between solved forearm and the
// hand's reference forearm axis. This is only a candidate: the posture solver
// must enforce its anatomical domain and compare roll and wrist bend together.
// It must not replace that domain with a wrist-only optimum. Near extension
// the posture solver retains its body-relative plane; the hand target is fixed.
WristPole WristAlignedPole(const Reference& reference, const FVector& shoulder,
                           const FVector& wrist, const FQuat& hand_world);

// Anatomical forearm turn (pronation/supination) from the bend plane and the
// palm, independent of any mesh's reference roll. upper = shoulder->elbow,
// forearm = elbow->wrist, palm = direction out of the palm, all in one space;
// bend_pole is the elbow's direction away from the shoulder->wrist line (used
// when the arm is straight). side_sign: +1 right arm, -1 left arm. Returns
// radians: 0 = palm toward the biceps side of the bend (fully supinated),
// pi/2 = thumb up (palm toward the body's midline), +/-pi = palm away (fully
// pronated), -pi/2 = palm facing outward (out of reach without the shoulder).
// NaN when the palm lies along the forearm.
double ForearmRotation(const FVector& upper, const FVector& forearm, const FVector& palm,
                       const FVector& bend_pole, double side_sign);
// The same measure for the skeleton's reference pose: the hand's palm axis
// (hand space) through the reference forearm-to-hand relation. NaN if the
// reference has no usable palm. A raw WristTwist of
// side_sign*(ForearmRotation - this) is that anatomical turn.
double ReferenceForearmRotation(const Reference& reference, const FVector& palm_axis_hand,
                                double side_sign);

// A fractional roll has two possible branches at the principal-angle seam.
// Within ONE authored support attachment, retain the observed palm path using
// bounded bone angles. This is intentionally path-dependent, not a globally
// history-independent fractional rotation. Clear on release/reattach/reset.
struct SupportTwistState {
    bool valid = false, observable = false;
    double source_radians = 0, target_radians = 0, forearm_radians = 0, share = 0;
};
struct SupportTwistResult {
    bool valid = false, observable = false;
    double source_radians = 0, target_radians = 0, forearm_radians = 0;
};
// Authored hand and anatomical palm define a geometric roll cue. A palm nearly
// parallel to the solved forearm cannot define roll: retain the last bounded
// result, then rebase at reacquisition instead of guessing the missing turn.
// On first observable attachment, seed the applied angle from the existing
// fallback; keep that frame exact, then approach the new target at a bounded
// rate. The seed is ignored after the attachment has initialized.
SupportTwistResult FollowSupportTwist(const Reference& reference,
    const FQuat& unrolled_forearm, const FQuat& authored_hand,
    const FVector& palm_axis_hand, double dt, double share, SupportTwistState* state,
    double initial_forearm_radians = 0);

struct TwistState {
    bool valid = false;
    double forearm_radians = 0, twist_radians = 0, watch_radians = 0;
    int seam_sign = 0;
    double seam_seconds = 0, seam_vote = 0;
    SupportTwistState support;
};
struct TwistResult {
    bool valid = false;
    double raw_radians = 0, target_radians = 0;
    double forearm_radians = 0, twist_radians = 0, watch_radians = 0;
    bool seam_guarded = false;
};
// Software seam guard, not a claimed anatomical joint limit. The elbow solver
// should prefer physical frames whose required roll is away from +/-pi.
constexpr double kTwistTargetLimit = 175.0 * kDegToRad;
constexpr double kTwistBoneRate = 360.0 * kDegToRad; // maximum applied bone rotation/second
// Multiply the bounded target by each share BEFORE smoothing the actual bone
// rotations. Dividing an unwrapped angle makes equivalent 248/-112 degree
// poses differ by 180 degrees at half share. No integer turns are stored here.
// Near the principal seam a short voting window rejects alternating signs;
// it never indefinitely preserves an old branch against a held input. A
// fractional roll cannot be globally continuous and history-independent over
// full turns: persistent crossings settle by a rate-limited branch change.
// Full-share bones take the short quaternion-equivalent arc independently.
// Apply returned bone angles directly to the unrolled frame, with no further
// sharing. Reset state on tracking/origin/rig changes.
// center_radians puts the seam opposite that roll instead of opposite the
// reference pose: pass the raw roll of a thumb-up forearm so the seam sits
// where the palm faces outward, a turn the forearm cannot make by itself.
// The target stays within kTwistTargetLimit of the center. watch_share is a
// third, independent channel (negative: the forearm share) for accessories
// that keep their own share of the turn.
TwistResult FollowTwist(double target_radians, double dt, double forearm_share,
                       double twist_share, TwistState* state, double center_radians = 0,
                       double watch_share = -1);

struct Attachment {
    bool valid = false;
    double along = 0;
    FVector off_axis_local;
    skel::Xf local;
};
// Twist skin bones may be sibling branches, not ancestors of the hand. Keep
// their reference fraction along the segment when calibrated lengths differ
// from the asset. An attachment outside the segment is unsupported, not clamped.
Attachment MeasureAttachment(const skel::Xf& start, const skel::Xf& end,
                             const skel::Xf& attachment);
// start is the chosen segment frame before any attachment-specific roll.
// Failure leaves *out untouched. Apply the resulting transform absolutely;
// no inherited-roll subtraction or assumed hierarchy is needed.
bool PlaceAttachment(const Attachment& reference, const skel::Xf& start,
                     const FVector& end, skel::Xf* out);

}  // namespace holevr::vr::arm_pose
