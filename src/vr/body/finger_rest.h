#pragma once
// Fingers that give way against a surface instead of passing through it
// (owner's feature list, 2026-11-03, item 13: "physical fingers"; reworked
// after the owner's hand-physics reference, SentientArt/TheBasement, whose
// hands are ragdolls pulled toward the controller's pose by springs).
//
// When an empty hand is against a wall or table (vr/body/hand_walls), each
// finger is pushed out of the surface the way a sprung finger would be: one
// pressed on its pad bends back (past straight at the knuckle, like a real
// finger pushed flat), one pressed on its nails curls. A fingertip that
// slides along the surface trails behind (friction). The surface is a plane
// from the hand's wall check, so none of this needs traces of its own.
// Pure math, unit tested in tests/test_finger_rest.cpp.

#include "vr/body/finger_solve.h"
#include "vr/body/skeleton.h"

namespace holevr::vr::fingers {

// How far the finger posed by `a` (radius `radius`, cm) goes past the plane
// through `point` with `normal` pointing out of the surface: 0 when clear.
// Joints and fingertip are checked. `hand`: the hand bone's world transform.
double PlaneDepth(const Chain& c, const Angles& a, const skel::Xf& hand, const FVector& point,
                  const FVector& normal, double radius);

// One number for how a finger is bent: 0 straight, 0..1+ curled as Curl(),
// below 0 bent back, to -1 at the joints' backward limits (thumb: half of
// them), most of it at the knuckle.
Angles Bend(const Chain& c, double bend, const Limits& lim);

struct Push {
    double bend = 0.0;    // the bend used
    double depth = 0.0;   // how far that still goes in, cm (0 = clear)
    int dir = 0;          // where the surface left it against the wanted bend: -1 more open, +1 more curled
    bool pushed = false;  // the hand had moved it into the surface and it was pushed out
};

// Where a finger the controller wants bent `want` (-1..max_curl) ends up
// against a surface, like a sprung finger, moving from where it was last
// frame (`from`). Clear of the surface, it bends toward `want` until it
// touches and rests there. If the hand has moved it into the surface, the
// surface pushes it out the way it presses on it, to the first bend that
// clears (or, when none does, the one that goes in least on that side):
// pressed on its pad it bends back, past straight at the knuckle; pressed
// on its nails it curls; straight on, a straight finger bends back and a
// curled one buckles further. `prefer` (last frame's dir) keeps a near tie
// from flipping between the two.
Push PushFromPlane(const Chain& c, const Limits& lim, const skel::Xf& hand, double from, double want,
                   double max_curl, const FVector& point, const FVector& normal, double radius, int prefer = 0);

// Fingertip drag: the change of bend and spread (radians) that moves the
// fingertip of the finger bent `bend` by `move` (world cm) as nearly as
// small changes can; clamped to +-0.3 bend and the joint's spread range.
void DragTip(const Chain& c, const Limits& lim, const skel::Xf& hand, double bend, const FVector& move,
             double* d_bend, double* d_spread);

// The posed fingertip in the world.
FVector TipWorld(const Chain& c, const Angles& a, const skel::Xf& hand);

}  // namespace holevr::vr::fingers
