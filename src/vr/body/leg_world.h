#pragma once
// Reflected game-thread queries only. Never moves the character, changes
// collision, or drives the game's kick timeline/animation.
#include <vector>
#include "ue/objects.h"
#include "vr/body/leg_pose.h"
#include "vr/body/stair_pose.h"

namespace holevr::vr::body::leg_world {
struct MovementState {
    bool known = false;
    bool grounded = false;
    bool kicking = false;
    bool floor_known = false;
    double floor_z = 0; // current scaled collision-capsule bottom, independent of animated bones
};
MovementState ReadState(ue::UObject player);
struct TerrainOffsets {
    bool known = false;
    FVector foot_location[2], pelvis_location;
    double pelvis_alpha = 0, trace_above = 0;
};
// Optional diagnostics from the player's actual animation instance. Values
// are reported as stored; their graph alpha/space is not assumed or subtracted.
// This never disables ALS foot IK or changes its offsets.
TerrainOffsets ReadTerrainOffsets(ue::UObject skeletal_component);

struct ProbeDiagnostics {
    unsigned traces_last=0, traces_peak=0;
    std::uint64_t traces_total=0, scans_accepted=0, scans_rejected=0, stair_feet=0;
};
struct ProbeState {
    bool scanning=false, have_origin=false;
    unsigned next_sample=0;
    double scan_age=0, profile_age=0, retry_seconds=0;
    uintptr_t player=0;
    FVector last_origin,scan_origin,scan_direction;
    double scan_floor=0;
    std::array<stairs::Sample,stairs::kSamples> samples{};
    stairs::Profile profile;
    ProbeDiagnostics diagnostics;
};
// One game-thread state per body. Maximum 9 trace calls: 3 amortized profile
// rays plus up to 3 fresh footprint rays per foot. Profiles are never used
// before their complete scan validates, and expire after 0.75 seconds.
// Failed scans wait 0.3s while idle or 0.15s while moving before retrying.
// Stair rays use complex query geometry; ordinary floor probes retain simple
// collision. Meshes without queryable treads fall back to source animation.
// Unknown state, airborne motion, kicking and discontinuities clear contacts.
void ProbeFeet(ProbeState& state,ue::UObject player,const legs::Input& input,
               legs::Ground (&ground)[2],const std::vector<ue::UObject>& extra_ignore = {});
} // namespace holevr::vr::body::leg_world
