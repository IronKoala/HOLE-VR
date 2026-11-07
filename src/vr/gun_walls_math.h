#pragma once
// The held gun's whole shape against walls. Pure math, unit tested in
// tests/test_gun_walls.cpp.
//
// Owner 2026-11-07: "the current physics hands and guns [should] collide
// correctly and not sink into the wall" (the hub). Until now only the line
// from the eyes to the muzzle was checked (Half-Life: Alyx style), so the
// sides of the slide, the grip and magazine, a rifle's stock and handguard
// and the hands holding the gun went into walls freely: a gun laid along a
// wall, or a rifle turned so its stock meets one, sank in.
//
// Now the outline of the gun's measured parts, and of the hands on it, is
// checked. Every outline point is looked at from the eyes; a point behind a
// surface (seen from the eyes) means that part of the gun is in it, and the
// whole gun moves out along that surface's normal, by the deepest point's
// depth. A second pass handles a corner between two surfaces.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "vr/math.h"

namespace holevr::vr::gun_walls {

// Gun frame: X along the barrel, Y right, Z up; cm from the firing hand's
// grip point.
struct Box {
    FVector lo, hi;
};

enum class Part : int { kGun = 0, kFiringHand, kMagazine, kSupportHand, kCount };
const char* PartName(Part part);

struct PartBox {
    Box box;
    Part part = Part::kGun;
};

struct Point {
    FVector at;  // gun frame
    Part part = Part::kGun;
};

// The firing hand wrapped round the grip: fingers in front of it, the palm's
// heel behind, knuckles and thumb to each side, from under the slide down to
// the little finger. Gun frame, from the grip point.
Box FiringHandBox();
// The support hand wrapped round a handguard or foregrip, drawn at `draw`
// (gun frame).
Box SupportHandBox(const FVector& draw);

// Points on the outline of the boxes, gun frame:
//  - for each of the 26 directions out of a cube (6 faces, 12 edges,
//    8 corners), the middle of the box corners furthest that way (the
//    muzzle's face, the top of the slide, the bottom of the magazine, the
//    knuckles...). Only real corners count, so a pistol's empty space under
//    the barrel never gets a point;
//  - a long gun (over 1.5 slices long) also gets its widest point to each
//    side, above and below in every slice of `slice_cm` along the barrel, so
//    a wall edge meeting the middle of a rifle is seen too.
// Points closer than merge_cm to one already kept are dropped (faces are
// kept first, then edges, corners and slices), at most max_points.
std::vector<Point> Outline(const std::vector<PartBox>& boxes, double merge_cm, double slice_cm,
                           std::size_t max_points);

// The push that brings `point` back in front of a surface (a point on it and
// its normal, facing the eyes): its depth behind the surface plus clearance,
// along the normal, at most max_cm. Zero when the point is in front.
FVector PushOut(const FVector& point, const FVector& impact, const FVector& normal, double clearance_cm,
                double max_cm);

// The smallest sphere round the points (not exact: centre of their box and
// the furthest point from it).
void Bounds(const std::vector<FVector>& points, FVector* centre, double* radius);

// One line from the eyes to a point, against the world.
struct Hit {
    bool hit = false;          // something is in the way
    bool eyes_inside = false;  // the line started inside something
    FVector impact, normal;
    std::uintptr_t what = 0;   // the thing hit (for the log)
};
using LineFn = std::function<Hit(const FVector& from, const FVector& to)>;

// Anything at all within `radius` of the path from `from` to `centre` (a
// ball swept along it). Every line from `from` to a point within `radius` of
// `centre` lies inside that sweep, so when it is clear none of those lines
// can meet anything. True when something is there or it can't be told.
using CloseFn = std::function<bool(const FVector& from, const FVector& centre, double radius)>;

// Splits the points into groups along the barrel, each about group_cm long
// (at most max_groups), so a wall beside one end of a rifle doesn't make
// every point be looked at. Group index per point.
std::vector<int> GroupsAlongBarrel(const std::vector<Point>& points, double group_cm, int max_groups);

struct Result {
    FVector push;              // add to the gun's location
    bool eyes_inside = false;  // the eyes were inside something: no push (as before)
    int queries = 0;           // lines looked along
    int groups_close = 0;      // groups with something near them (first pass)
    int pushed_points = 0;     // points behind something on the first pass
    int deepest = -1;          // the first pass's deepest point (index), -1: none
    Part deepest_part = Part::kGun;
    std::uintptr_t what = 0;   // what the first pass's deepest point was behind
};

// Up to `passes` passes: each looks at every point (world, already moved by
// the push so far) from the eyes and moves the gun out of the surface the
// deepest one is behind. Stops early when a pass finds nothing. The push is
// at most max_cm in all. With `close`, a group of points (`groups`, one
// group when empty) is only looked at when something is near the path from
// the eyes to it.
Result Solve(const FVector& eyes, const std::vector<FVector>& points, const std::vector<Part>& parts,
             const LineFn& line, double clearance_cm, double max_cm, int passes,
             const std::vector<int>& groups = {}, const CloseFn& close = {});

}  // namespace holevr::vr::gun_walls
