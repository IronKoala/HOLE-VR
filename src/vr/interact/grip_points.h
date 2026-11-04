#pragma once
// Where the support hand holds a long gun's handguard. Owner (2026-11-04,
// first run with the AR): "I only want the grip points, one in front of the
// magwell, one behind the flashlight and one where the picatinny rail ends."
// So a left squeeze on the handguard takes the nearest of those three
// points, and the hand is drawn there, instead of wherever the controller
// happened to be. Each point has its own hand pose in the grip editor.
//
// Pure math (no game calls), unit tested in tests/test_grip_points.cpp.
// vr/controls measures the gun's own parts (the handguard and its rails, the
// lower receiver, an installed light or laser) and uses the points for the
// two-hand hold and for the rifle held by the left hand alone
// (docs/RIFLE_CARRY.md).
//
// Gun frame: X barrel (forward), Y right, Z up, origin the grip point
// (MainHandPosition). Lengths in cm.

#include "vr/math.h"

namespace holevr::vr::interact {

enum GripPointId { kPointMagwell = 0, kPointLight = 1, kPointRailEnd = 2, kPointCount = 3 };

// Short names: grip files are <Gun class>.support-<name>.ini.
const char* PointKey(int id);
// For the log.
const char* PointName(int id);

// The gun's parts, measured in the gun frame. A part that was not found has
// its `have_` flag false.
struct GunParts {
    bool have_handguard = false;
    FVector hg_lo, hg_hi;          // box round the handguard mesh
    bool have_rails = false;
    double rail_front = 0;         // front end of the picatinny rails on the handguard
    bool have_magwell = false;
    double magwell_front = 0;      // front end of the lower receiver (the magwell)
    bool have_light = false;
    double light_rear = 0;         // back end of a light or laser on the handguard
};

struct GripPoints {
    bool ok = false;               // false: no handguard found; the hand holds where it grabs
    bool have[kPointCount] = {};
    bool light_found = false;      // the middle point is behind a light (else halfway)
    FVector p[kPointCount];        // where the hand holds (its grip point), gun frame
    // The grab zone round the handguard.
    FVector zone_lo, zone_hi;
};

// hand_half: how far the hand's grip point sits from the edge it is against
// (half a hand's width). side_gap: how far outside the handguard's left side
// the grip point sits (the palm against it, as the owner held it in the
// first run: about 5 cm left of the bore line).
GripPoints MakeGripPoints(const GunParts& parts, double muzzle_dist, double hand_half = 4.5, double side_gap = 2.5);

// The point for a left grab at `grab` (gun frame): the nearest one along
// the barrel, if the grab is in the handguard zone. -1: not on the handguard.
int NearestPoint(const GripPoints& g, const FVector& grab);

// A squeeze held for a long time (the support hand on the handguard, the
// right hand on a rifle the left hand also holds). It ends only after the
// squeeze stays under `release` for `grace` seconds, so a hand that relaxes
// a little while aiming keeps its hold; an open hand lets go at once (the
// grace is a few frames).
struct LongSqueeze {
    double below_for = 0;
    float lowest = 1.f;            // lowest squeeze while held (for the log)
    void Start() { below_for = 0, lowest = 1.f; }
    // true: still held.
    bool Update(float value, double dt, float release = 0.2f, double grace = 0.12);
};

}  // namespace holevr::vr::interact
