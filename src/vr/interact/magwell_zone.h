#pragma once
// Where a magazine lines up with (snaps onto) a gun's magwell, measured for
// each magazine and gun from their meshes (owner 2026-11-06: "that happens
// way to early, i think you made the detection box based off of another mag
// and re-used it for all AR mags, if so make custom sized detection zones
// for all mags").
//
// Before, one fixed window was used for every rifle magazine (lined up when
// its origin was up to 16 cm below its seat) and one for every pistol
// magazine (14 cm). The AR's 10 round magazine only goes 9.6 cm into the
// lower receiver, so it snapped with its top still ~5 cm below the magwell.
//
// Now: the magazine's top (its mesh, turned as it sits seated) and the
// magwell's opening (the bottom of the gun part the magazine's axis runs up
// into: an AR's lower receiver, a pistol's frame) give how far the
// magazine travels inside the gun. It snaps when its top is within `slack`
// of the opening, and the hand's push from there to seated is the reload's
// progress (0 at the opening, 1 seated), which poses the hand.
//
// Gun frame: X barrel, Y right, Z up, cm. Pure math, unit tested in
// tests/test_magwell_zone.cpp.

#include <string>
#include <vector>

#include "vr/math.h"

namespace holevr::vr::interact {

struct PartBox {
    std::string name;
    FVector lo, hi;  // the part's mesh box, gun frame
};

struct MagwellZone {
    bool ok = false;
    double top = 0;      // the seated magazine's top, along the axis from where its origin sits seated
    double bottom = 0;   // its bottom, the same way (negative: below the seat)
    double opening = 0;  // the magwell opening, the same way
    double insert = 0;   // top - opening: how far the magazine travels inside the gun
    std::string part;    // gun part whose bottom is the opening
};

// mag_corners: corners of the magazine's mesh boxes as it sits seated, gun
// frame. seat: where its origin sits seated. in: unit direction up into the
// magwell. parts: the gun's own visible parts (not the magazine).
MagwellZone MeasureMagwell(const std::vector<FVector>& mag_corners, const FVector& seat, const FVector& in,
                           const std::vector<PartBox>& parts);

// How far below its seat the magazine's origin may be for it to line up:
// its top within `slack` of the opening. `fallback` when not measured.
double SnapBelow(const MagwellZone& z, double slack, double fallback);

// Where the hand pushing the magazine finishes it by itself (the seat
// animation), cm below the seat: `finish`, but never more than half of the
// travel inside the gun.
double SeatFinish(const MagwellZone& z, double finish);

// The reload's progress for a magazine whose origin is `below` cm under its
// seat: 0 with its top at the opening (or lower), 1 seated.
double InsertProgress(const MagwellZone& z, double below, double fallback_insert);

}  // namespace holevr::vr::interact
