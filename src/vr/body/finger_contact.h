#pragma once
// Local, per-phalanx kinematic contact response. World queries provide finite
// tangent patches; no query or engine call occurs inside this bounded solver.

#include <array>
#include <cstdint>

#include "vr/body/joint_limits.h"

namespace holevr::vr::finger_contact {

struct Contact {
    enum class Source { desired, displayed, temporal, self };
    bool valid = false;
    int segment = 0;             // 0 base->middle, 1 middle->distal, 2 distal->tip
    double fraction = 1.0;       // contact sample along that centerline, 0..1
    FVector point{}, normal{};   // surface point and outward normal, world cm
    double radius_cm = 0.5;      // finger collision radius
    double patch_radius_cm = 2.0; // validity radius along the surface
    double age_seconds = 0.0;
    Source source = Source::desired;
    std::uint64_t surface_id = 0;
    bool recovered_overlap = false;
    bool point_sample_only = false; // capsule-pair closest point, not a world patch
};
constexpr int kMaxContacts = 16; // world observations plus all four other digits
struct Contacts {
    std::array<Contact, kMaxContacts> items{};
    int count = 0;
};
struct Bounds {
    joint_limits::Axes axes[3];
    joint_limits::Limits limits[3];
};
// Runtime should use the rig's existing reference-relative axes and limits.
Bounds MakeBounds(const fingers::Chain& chain, int finger);

struct State {
    bool initialized = false;
    fingers::Angles pose{};
    fingers::Angles frame_start{};
    std::array<double, 9> angular_velocity{}; // flex/side/twist per joint, radians/s
    double frame_seconds = 0, frame_max_step = 0;
    bool frame_has_prior = false;
};
struct Result {
    fingers::Angles pose{};
    double residual_cm = 0.0;
    int active_contacts = 0;
    int iterations = 0;
    bool limited = false;
};
void Reset(State& state);

// The inputs' desired pose is never overwritten. Joint coupling is only a
// weak pose preference: each of the three flexion angles can resolve contact
// independently. Easing precedes collision resolution; callers must apply
// the returned pose without another unconstrained visual blend.
Result Solve(const fingers::Chain& chain, const fingers::Angles& desired,
             State& state, const skel::Xf& hand, const Bounds& bounds,
             const Contacts& contacts, double dt, bool reset = false);

// Additional same-frame constraints share Solve's original angular budget;
// no second input easing or extra per-frame movement allowance is granted.
Result ResolveContacts(const fingers::Chain& chain, State& state,
                       const skel::Xf& hand, const Bounds& bounds, const Contacts& contacts);

// Shared FK/contact helpers for regression tests and runtime diagnostics.
FVector SampleWorld(const fingers::Chain& chain, const fingers::Angles& pose,
                    const skel::Xf& hand, int segment, double fraction);
double Depth(const fingers::Chain& chain, const fingers::Angles& pose,
             const skel::Xf& hand, const Contacts& contacts, int* active = nullptr);

}  // namespace holevr::vr::finger_contact
