#pragma once
// Bounded, collision-constrained hand proxy. This is a virtual spring model:
// it does not create Chaos rigid bodies or apply impulses to game props.
#include <functional>
#include "vr/body/ik_math.h"

namespace holevr::vr::body::hand_motion {

struct Settings {
    double radius_cm = 4.0, skin_cm = 0.03;
    double frequency_hz = 10.0, damping_ratio = 1.0;
    double virtual_mass_kg = 0.35, max_force_n = 65.0, max_speed_cm_s = 700.0;
    double angular_frequency_hz = 12.0, virtual_inertia = 0.002;
    double max_torque_nm = 1.2, max_angular_speed = 18.0; // radians/s
    double friction_per_s = 5.0;
    double recovery_distance_cm = 40.0, recovery_seconds = 0.25;
    double discontinuity_cm = 80.0, max_frame_seconds = 0.10;
    int max_queries = 24; // Includes translation, slide, rotation clearance and seating.
};

struct Sweep {
    bool available = false, hit = false, initial_overlap = false;
    FVector centre, point, normal;
};
using SweepFn = std::function<Sweep(const FVector&, const FVector&, double)>;
// Fixed-orientation box sweep: centres, outer half extents, world rotation.
using BoxSweepFn = std::function<Sweep(const FVector&, const FVector&, const FVector&, const FQuat&)>;

struct PalmShape {
    bool valid = false;
    FVector centre;  // relative to the raw grip, including calibrated wrist mapping
    FQuat rotation;  // local X across palm, Y toward fingers, Z out of palm
    FVector half;    // outer half extents; Z is palm half thickness
    bool flatten = true;
    double press_cm = 4.0;
};
struct RootSupport {
    FVector point, normal, local_sample; // witness centre in grip-local space
    double radius = 0, remaining_seconds = 0;
};

struct Input {
    bool tracked = true, discontinuity = false;
    FVector target, anchor; // desired fist and a known tracking origin (head), cm
    FQuat rotation;
    double dt = 0;
    PalmShape palm;
    const RootSupport* supports = nullptr;
    int support_count = 0;
};
struct State {
    bool valid = false, touching = false;
    FVector position, velocity, angular_velocity, last_target, last_anchor, normal;
    FQuat rotation;
    double blocked_seconds = 0;
};
struct Result {
    bool valid = false, touching = false, sliding = false, recovering = false;
    bool reset = false, query_failed = false;
    bool budget_limited = false, rotation_blocked = false;
    bool palm_shape = false;
    FVector position, point, normal;
    FQuat rotation;
    double error_cm = 0, speed_cm_s = 0, peak_force_n = 0, peak_torque_nm = 0;
    double flatness = 0; // fresh pressure and actual checked palm-facing alignment
    int queries = 0, substeps = 0;
};

void Reset(State& state);
// A fresh query is made even at rest. The callback must distinguish failed
// queries from clear space. On reacquisition, anchor->target is swept before
// placing the proxy; a blocked hand is never snapped to its raw target.
Result Step(State& state, const Settings& settings, const Input& input, const SweepFn& sweep,
            const BoxSweepFn& box_sweep = {});
// Check a requested rigid correction/final IK pose from an already acquired
// proxy. No spring lag or tracking reset; returns a blocked/partial safe pose
// when necessary. Failure is explicit and does not commit tentative motion.
Result Constrain(State& state, const Settings& settings, const PalmShape& palm,
                 const FVector& position, const FQuat& rotation, const SweepFn& sweep,
                 const BoxSweepFn& box_sweep = {}, const RootSupport* supports = nullptr, int support_count = 0);

} // namespace holevr::vr::body::hand_motion
