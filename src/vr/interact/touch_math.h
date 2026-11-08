#pragma once
// Touch interact maths: what a hand resting on a thing means, without any
// game calls. Unit tested in tests/test_touch.cpp; vr/interact/touch.cpp
// reads the game and calls the game's own interact.
//
// Owner 2026-11-03: "to pick up loot and open loot crates (fridge and box for
// example) i'd like to just place my hand on the item, to open doors i'd like
// the ability to just place my hand on the door handle". No body-motion
// gestures (project rule): resting the hand on the thing is the whole action.

#include <cmath>
#include <string>
#include <vector>

#include "vr/math.h"

namespace holevr::vr::touch {

// What a game interactable is, for touch.
//   kSkip:      never by touch (the hub drink machine: the drinks code owns it)
//   kOther:     the trigger still interacts with it, a touch does nothing
//   kLoot:      a pickup (BP_ItemBase_C): touching it takes it
//   kContainer: a loot box or fridge (BP_ItemBoxBase_C, LCBP_Fridge_C): touching opens it
//   kDoor:      a door (BP_DoorBase_C): touching its handle opens or closes it
enum class Kind : int { kSkip, kOther, kLoot, kContainer, kDoor };
const char* KindName(Kind k);

// From the class name and its parents' names, own class first.
// `extra_containers` and `skip`: class-name pieces from hole-vr.ini.
Kind Classify(const std::vector<std::string>& chain, const std::vector<std::string>& extra_containers,
              const std::vector<std::string>& skip);

// Comma-separated list, trimmed, empties dropped.
std::vector<std::string> SplitList(const std::string& s);

// An axis-aligned box in some frame (a component's frame with its scale
// already applied, so the numbers are cm).
struct Box {
    FVector lo, hi;
};
double DistToBox(const FVector& local, const Box& b);  // 0 inside
double BoxVolume(const Box& b);
Box Grow(const Box& b, double cm);
// A world point in a frame at `origin` turned by `rot`.
inline FVector ToFrame(const FQuat& rot, const FVector& origin, const FVector& world) {
    return rot.Inverse().Rotate(world - origin);
}

// Aim assist for the trigger (owner 2026-11-04: opening boxes with the
// trigger "still really unreliable"; ten presses at boxes about 1 m away all
// missed, because only an exact hit on a small dome at the box's base
// counted). The closest the aim segment from `o` along unit `d`, `len` long,
// comes to box `b`: returns the gap in cm (0 when the segment goes through
// the box) and in *t_at how far along the segment that is (where it enters,
// when it goes through).
double SegmentBoxGap(const FVector& o, const FVector& d, double len, const Box& b, double* t_at);
// A gap `gap` cm beside the aim, `t` cm out, is inside the aim cone: within
// `cone_deg` of the aim seen from the hand, or within `min_cm` of it.
inline bool InAimCone(double gap, double t, double cone_deg, double min_cm) {
    return gap <= std::fmax(min_cm, std::tan(cone_deg * 3.14159265358979323846 / 180.0) * std::fmax(t, 0.0));
}

// Where a hand points, from its controller's aim pose turned by `pitch_deg`
// and `yaw_deg` in the controller's own frame (yaw mirrored for the left
// hand, `left`). The aim pose sits about 30 degrees above where a hand
// naturally points, so the default is pitch -30, yaw 5 (vr/controls' pistol).
FVector PointDir(const FQuat& aim, double pitch_deg, double yaw_deg, bool left);

// The box of a mesh's own bounds [mn, mx] under a component scale (negative
// scales flip an axis; the result is sorted).
Box ScaledBox(const FVector& mn, const FVector& mx, const FVector& scale);

// A door's handle zone, in the leaf's (scaled) frame, from the leaf's box in
// that frame, the hinge point in that frame and the world's up direction in
// that frame. The handle is on the edge away from the hinge, `height` above
// the leaf's bottom; the zone runs `from_edge` in from that edge (and `past`
// beyond it) and `out` past both faces, so a hand resting on the handle on
// either side is inside it. Without a hinge (hinge_known false) both edges
// get a zone. Returns how many zones (0 when the leaf doesn't look like a door).
struct HandleZoneSettings {
    double height = 100.0;      // handle height above the leaf's bottom, cm
    double half_height = 20.0;  // zone half height, cm
    double from_edge = 22.0;    // zone width in from the far edge, cm
    double past = 4.0;          // and past the edge, cm
    double out = 12.0;          // past each face, cm
};
int DoorHandleZones(const Box& leaf, const FVector& hinge, bool hinge_known, const FVector& up_in_leaf,
                    const HandleZoneSettings& s, Box zones[2]);

// One hand on one thing, over time. Step it every tick with the hand's
// distance to the thing (cm, 0 = touching or inside):
//   - a touch needs the hand to come from outside: a thing first seen with the
//     hand already on it (an item that popped out of a box into the hand)
//     waits until the hand has left it once;
//   - the hand must stay on it `hold_s` (a hand brushing past does nothing);
//   - after a touch the hand must leave (past `leave_cm`) before the next one.
// Returns true on the tick the touch fires.
struct TouchState {
    bool seen = false;
    bool armed = false;
    bool inside = false;
    double since = 0.0;
};
bool StepTouch(TouchState& s, double dist, double now, double touch_cm, double leave_cm, double hold_s);
// The hand touched something else (it fired there): this one waits until the
// hand leaves it, so one touch never does two things.
inline void Disarm(TouchState& s) { s.armed = false; }
// Where the hand is is not known this tick (tracking lost, a menu or pause,
// the hand busy holding something) and it is not plainly away. That is not a
// leave: a touch already used stays used until the hand is seen away. A rest
// in progress starts again, so the hand must rest the full time once it is
// seen on the thing again.
inline void StepUnknown(TouchState& s) { s.inside = false; }

}  // namespace holevr::vr::touch
