#pragma once
// Local, per-phalanx kinematic contact response. World queries provide finite
// tangent patches; no query or engine call occurs inside this bounded solver.

#include <array>
#include <cstdint>

#include "vr/body/joint_limits.h"

namespace holevr::vr::finger_contact {

struct Contact {
    enum class Source { desired, displayed, temporal, self, endpoint };
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
// Empty-hand input uses anatomical curl, compensating the relaxed reference
// bends. Authored weapon/grip records must retain their existing targets.
fingers::Angles CurlTarget(const fingers::Chain& chain,double amount,const Bounds& bounds);

struct State {
    bool initialized = false;
    fingers::Angles pose{};
    fingers::Angles frame_start{};
    fingers::Angles frame_intent{}; // uncluttered input goal for self-release coordination
    fingers::Angles self_anchor{}, self_intent{};
    bool self_yield_active = false;
    unsigned self_search_cursor = 0; // hand-wide search uses slot zero's cursor
    unsigned self_search_tick = 0;
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
    bool blocked = false, root_required = false;
};
void Reset(State& state);

// Establish the one frame budget without consuming it. Query this candidate,
// then pass the self-safe candidate to ResolvePrepared; do not ease twice.
fingers::Angles Prepare(const fingers::Chain& chain,const fingers::Angles& desired,
                        State& state,const Bounds& bounds,double dt,bool reset=false,bool already_prepared=false);
fingers::Angles Preview(const fingers::Chain& chain,const fingers::Angles& desired,
                        const State& state,const Bounds& bounds,double dt);
Result ResolvePrepared(const fingers::Chain& chain,const fingers::Angles& candidate,
                       State& state,const skel::Xf& hand,const Bounds& bounds,const Contacts& contacts);

struct Witness {
    bool active=false;
    FVector center{}, surface{}, normal{};
    double radius_cm=0, depth_cm=0, fraction=0;
    int segment=0;
};
// Deepest current capsule support inside a fresh local surface patch. Rigid
// hand response must verify its proposed displacement against world geometry.
Witness EvaluateContact(const fingers::Chain& chain,const fingers::Angles& pose,
                        const skel::Xf& hand,const Contact& contact);

// Compatibility wrapper: Prepare then ResolvePrepared. Contact arrests motion
// along input intention; it does not invent deep flexion to clear an obstacle.
// Unresolved depth requires checked hand-root backpressure.
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
