#pragma once
// Measuring a held gun with line traces and posing the hand on it.
//
// The game's gun parts report no size (GetLocalBounds and the mesh boxes come
// back as zero), but line traces against them do hit. So the gun is scanned:
// a grid of rays across the gun (along its right axis) in the gun frame gives
// its side silhouette plus the right and left surface at every cell. From
// that: where the grip is, how it slants, where the trigger guard starts,
// and how wide it is. The hand is then placed from those measurements:
// palm on the right side of the grip, middle finger right under the trigger
// guard, knuckles at the front strap, thumb across the back to the left
// side. Pure math, unit tested in tests/test_body.cpp.
//
// Gun frame: X barrel (forward), Y right, Z up, origin MainHandPosition.

#include <cmath>
#include <string>
#include <vector>

#include "vr/body/ik_math.h"

namespace holevr::vr::grip {

struct Grid {
    double x0 = -12, z0 = -8, step = 0.5;
    int nx = 0, nz = 0;
    // Per cell (iz * nx + ix): Y of the right surface (ray from +Y side) and
    // of the left surface (ray from -Y side). NaN = the ray missed.
    std::vector<double> yr, yl;
    void Init(double x0_, double x1, double z0_, double z1, double step_);
    double X(int ix) const { return x0 + ix * step; }
    double Z(int iz) const { return z0 + iz * step; }
    bool Occ(int ix, int iz) const;
    // Inside the scanned solid? depth: how far inside (cm, toward the
    // nearer side surface).
    bool Inside(const FVector& p, double* depth = nullptr) const;
};

struct Shape {
    bool ok = false;
    std::string why;          // when not ok
    double z_bottom = 0;      // lowest grip row
    double z_guard = 0;       // bottom of the trigger guard (top of the free grip)
    double front = 0;         // grip front strap X just under the guard
    double back = 0;          // grip back strap X just under the guard
    double y_right = 0;       // grip right side Y (middle of the grip)
    double y_left = 0;        // grip left side Y
    FVector up;               // grip axis, pointing up the grip
    double slant_deg = 0;     // grip slant from vertical
    double length = 0, height = 0;  // whole gun silhouette, cm
    // Left side of the frame where the thumb rests: back of the frame at the
    // thumb's height (thumb_x) and the frame's left surface there (thumb_y).
    double thumb_z = 0, thumb_x = 0, thumb_y = 0;
    int cells = 0;  // occupied cells
};

// thumb_above: how far above the trigger guard bottom the thumb lies (cm).
Shape Analyze(const Grid& g, double thumb_above = 1.5);

// The avatar hand in its own bone space: pinky->index axis, palm normal,
// and the knuckle (finger base) positions from the wrist.
struct HandGeo {
    FVector across, palm;
    FVector mcp[5];  // thumb, index, middle, ring, pinky (hand_pose order)
};

// Points of the palm skin in hand-bone space (between the wrist and the
// finger knuckles, pushed out of the palm by palm_skin).
std::vector<FVector> PalmPoints(const HandGeo& h, double palm_skin);

struct Params {
    double palm_skin = 1.0;      // cm from the knuckle line out to the palm skin
    double finger_radius = 1.0;  // middle finger centre below the trigger guard
    double knuckle_back = 0.5;   // middle knuckle behind the front strap
    double clearance = 0.2;      // gap between palm and grip
};

struct Placement {
    FQuat rot;      // hand bone rotation in the gun frame
    FVector pos;    // hand bone (wrist) position in the gun frame
    FVector thumb;  // where the thumb's knuckle goes: back left corner of the frame
    FVector thumb_dir;  // direction the thumb lies along (forward)
    double pushed = 0;  // how far the palm had to be moved out of the grip, cm
};

// Right hand on a pistol grip. The grid is used to make sure no palm point
// ends up inside the gun.
Placement PlaceRightHand(const Grid& g, const Shape& s, const HandGeo& h, const Params& p);

// Deepest point of a polyline (gun frame) inside the scanned gun: returns
// the depth (0 = clear) and the sample's index along the line in *at.
double Penetration(const Grid& g, const std::vector<FVector>& pts, double radius, int* at = nullptr);

}  // namespace holevr::vr::grip
