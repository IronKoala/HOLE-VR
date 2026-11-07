#pragma once
// Contact-qualified visual leg planning. No engine access, body forces, or
// writes to the game's animation. Units are centimetres and seconds.
#include <cstdint>
#include "vr/body/skeleton.h"

namespace holevr::vr::body::legs {

struct Ground {
    bool hit = false;
    bool initial_overlap = false;
    FVector point, normal{0, 0, 1};
    uintptr_t support_id = 0;
    skel::Xf support_frame; // current component-to-world; no pointer is dereferenced here
    // Set only after a multi-tread profile and fresh centre/heel/toe traces
    // agree. An ordinary elevated hit must leave this false.
    bool stair = false;
    double clearance_z = 0; // current/next tread top for swing clearance
};

struct FootInput {
    bool valid = false;
    FVector hip;           // hip AFTER proposed body placement, before this plan's pelvis_offset
    FVector animated_hip;  // source animation, turned/translated with body, without crouch correction
    FVector knee, ankle;   // same animation space as animated_hip; preserves gait/kick data
    FQuat rotation;
    double upper_length = 0, lower_length = 0; // reference segment lengths after component scale, in world cm
    double sole_offset = 0; // reference ankle height above the root/floor, measured from the rig
    FVector sole_up_local; // reference foot rotation inverse * component up; zero keeps animation's rotation
    bool stance_valid = false;
    FVector stance_ankle, stance_pole{1,0,0}; // reference standing stance under current body, not terrain IK
    FQuat stance_rotation;
};
struct Input {
    double dt = 0;
    FVector origin;        // actor/world origin: detects teleports, excludes locomotion from foot speed
    FVector forward{1, 0, 0};
    FVector velocity;      // world movement velocity; heading for stair probes/gait phase
    double speed = 0;
    bool state_known = false, grounded = false, kicking = false;
    bool floor_known = false;
    double floor_z = 0;     // capsule bottom / actual character support; NEVER animated foot/pelvis height
    FootInput foot[2];
};
enum class Mode { Bypass, Animation, Planted, Step, Recovery, StairSwing, StairSupport };
struct FootOutput {
    bool solve = false;     // false: leave the copied local animation completely alone
    FVector ankle, pole;
    FQuat rotation;
    Mode mode = Mode::Bypass;
};
struct Output {
    FootOutput foot[2];
    FVector pelvis_offset; // apply BEFORE arms, then solve legs against output ankles
    unsigned steps_taken = 0;
};

struct FootState {
    bool previous = false, locked = false, stepping = false;
    FVector previous_local;
    bool source_previous = false, repairing = false;
    FVector previous_source_local;
    skel::Xf repair;
    double stable = 0, phase = 0;
    double stair_adjust_z = 0, contact_source_z = 0;
    bool step_on_stairs = false;
    skel::Xf planted_local, from, to_local;
    uintptr_t support_id = 0;
};
struct State {
    FootState foot[2];
    bool have_origin = false, moving = false;
    FVector origin;
    double settle = 0, action_settle = 0;
    int next_foot = 0;
    unsigned steps_taken = 0;
};

// Reject walls, overlaps, non-finite values and surfaces too far from the
// expected sole. A short sole-height probe cannot accidentally find a table
// above the knees. These limits are also applied to adapter results.
bool ValidGround(const Ground& ground, const FVector& expected_sole);
FVector ExpectedSole(const FootInput& foot);
// Probe from the physical character's floor height. A foot already lifted
// onto a table by the source animation must not turn the table into our floor.
FVector ProbeSole(const Input& input, int side);
// Upward-only repair of source terrain IK moving the whole pelvis onto
// furniture. Read before head/body fitting, using ORIGINAL foot snapshots.
// Reads state/floor/speed and foot valid/ankle/sole_offset only; dt and full
// leg geometry are unnecessary. Invalid/unknown evidence returns source_z.
double CorrectIdlePelvisHeight(const Input& snapshot, double source_z, double nominal_z);
void Reset(State& state);
Output Plan(State& state, const Input& input, const Ground (&ground)[2]);

} // namespace holevr::vr::body::legs
