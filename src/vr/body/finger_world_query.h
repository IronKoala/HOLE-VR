#pragma once
// Bounded, injectable world queries for finger capsule segments. No engine
// calls here: the runtime adapter and tests implement the same sweep API.

#include <array>

#include "vr/body/finger_contact.h"

namespace holevr::vr::finger_world_query {

constexpr int kFingers = 5, kSegments = 3;
struct Geometry {
    bool valid[kFingers]{};
    FVector point[kFingers][4]{};  // base, middle, distal, fingertip; world cm
    FVector wrist{};
    FQuat orientation{};
    double radius_cm = 0.5;
    double segment_radius_cm[kFingers][kSegments]{}; // zero uses legacy fallback above
};
Geometry MakeGeometry(const fingers::Chain chains[kFingers], const fingers::Angles poses[kFingers],
                      const skel::Xf& hand, double radius_cm);

struct Hit {
    FVector center{}, point{}, normal{};
    bool initial_overlap = false;
    std::uint64_t surface_id = 0; // reflected component identity, zero if unknown
};
enum class QueryStatus { unavailable, clear, hit };
struct Backend {
    virtual ~Backend() = default;
    virtual QueryStatus Sweep(const FVector& from, const FVector& to, double radius_cm, Hit& hit) = 0;
    // Optional rigid-feature tracking. Unsupported/deforming components must
    // return false. This never extends a geometry observation's lifetime.
    virtual bool SurfaceTransform(std::uint64_t, skel::Xf&) { return false; }
    virtual int InvocationCount() const { return -1; }
};
struct Settings {
    int max_casts = 32;               // strict bound, includes overlap recovery
    double clear_refresh_seconds = 0.05;
    double max_contact_age_seconds = 0.05;
    double movement_threshold_cm = 0.15;
    double teleport_distance_cm = 30.0;
};
struct Stats {
    bool available = false, nearby = false, reset = false, budget_limited = false;
    bool geometry_clear = false;
    int casts = 0, reused = 0, expired = 0, failures = 0, temporal_casts = 0, displayed_casts = 0;
    int primary_hits = 0, broad_false_clear = 0, recovered_overlaps = 0, failed_recovery = 0;
    int conflicting_contacts = 0;
    int surface_queries = 0, cache_hits = 0, feature_casts = 0, hit_results = 0, engine_calls = 0;
    // Missing required evidence: finger bits0..4 and segment bits(f*3+j).
    // Integration holds these digits; an unknown query is not clear space.
    unsigned unresolved_mask = 0, unresolved_segments = 0;
};
struct Observation {
    bool valid = false, clear = false, geometry_clear = false, have_frame = false;
    FVector from{}, to{};
    double radius_cm = 0, age_seconds = 1e9;
    skel::Xf frame{};
};
struct SegmentCache {
    // Candidate capsule, displayed capsule, temporal endpoint, deeper
    // spatial endpoint. Each contact
    // carries its own observation age; refreshing one cannot renew another.
    std::array<finger_contact::Contact, 4> contacts{};
    std::array<Observation, 4> observation{};
};
struct State {
    bool initialized = false, have_rendered = false, broad_clear = false;
    Geometry queried{}, rendered{};
    int cursor = 0, work_cursor = 0;
    int frame_casts = 0;
    SegmentCache segment[kFingers][kSegments]{};
};
void Reset(State& state);
// Commit only the actual rendered/solved geometry, after all pose changes.
void Commit(State& state, const Geometry& rendered);
// A checked whole-hand correction changes the queried geometry. Keep only
// feature-side hints for real recovery casts; do not reuse its clearance.
void InvalidateGeometry(State& state);
struct Verification {
    bool available = false, clear = false, budget_limited = false;
    int casts = 0, engine_calls = 0;
    unsigned blocked_mask = 0;
};
// Endpoint clearance only, after a root/pose correction. Shares Update's
// remaining cast budget and never advances contact ages. A hit is blocked;
// exhausted budget is unknown. It does not certify the swept motion path.
Verification VerifyGeometry(State& state, const Geometry& actual, Backend& backend,
                            const Settings& settings = {});
double MaxDelta(const Geometry& a, const Geometry& b);
Stats Update(State& state, const Geometry& requested, double dt, Backend& backend,
             finger_contact::Contacts output[kFingers], const Settings& settings = {});

}  // namespace holevr::vr::finger_world_query
