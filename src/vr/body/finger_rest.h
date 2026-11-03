#pragma once
// Fingers that rest on a surface instead of passing through it (owner's
// feature list, 2026-11-03, item 13: "physical fingers"). When an empty hand
// is against a wall or table (vr/body/hand_walls), each finger takes the
// curl closest to what the controller asks for that keeps it on the open
// side of the surface: an open hand pressed on a wall lies flat on it, and
// fingers pointed into a wall bend back against it. The surface is a plane
// from the hand's wall check, so this needs no traces of its own. Pure math,
// unit tested in tests/test_finger_rest.cpp.

#include "vr/body/finger_solve.h"
#include "vr/body/skeleton.h"

namespace holevr::vr::fingers {

// How far the finger posed by `a` (radius `radius`, cm) goes past the plane
// through `point` with `normal` pointing out of the surface: 0 when clear.
// Joints and fingertip are checked. `hand`: the hand bone's world transform.
double PlaneDepth(const Chain& c, const Angles& a, const skel::Xf& hand, const FVector& point,
                  const FVector& normal, double radius);

// The curl, 0..max_curl, closest to `want` that keeps the finger clear of the
// plane; when none does, the one that goes in least. *depth (optional): how
// far the chosen curl goes in.
double CurlClearOfPlane(const Chain& c, const Limits& lim, const skel::Xf& hand, double want, double max_curl,
                        const FVector& point, const FVector& normal, double radius, double* depth = nullptr);

}  // namespace holevr::vr::fingers
