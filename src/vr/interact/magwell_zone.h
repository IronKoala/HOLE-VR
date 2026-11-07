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

// How far below its seat the magazine's origin is when it lines up: its top
// `out_cm` under the magwell opening (owner 2026-11-07: "make it stick out of
// the mag well by an inch"). `fallback` when not measured.
double LineUpBelow(const MagwellZone& z, double out_cm, double fallback);

// Where the hand pushing the magazine in hands over to the seat animation,
// cm below the seat: `last` (the final click), never more than half of the
// travel inside the gun. Before 2026-11-07 it was 6 cm (half of a pistol
// magazine's travel), and the pistol magazine went in by itself half way up
// ("it snaps in way to early").
double SeatLast(const MagwellZone& z, double last);

// The reload's progress for a magazine whose origin is `below` cm under its
// seat: 0 where it lines up (`line_up` cm below), 1 where it seats itself
// (`last` cm below), so the hand's reload animation has played out by the
// time the magazine is fully in (owner 2026-11-07).
double InsertProgress(double below, double line_up, double last);

// The held magazine's top against the magwell. The line-up test used the
// magazine's origin, which on the AR's magazines is 17 cm under its top: a
// magazine tilted 30-60 deg or 7 cm to the side still lined up with its top
// far from the magwell.
struct TopProbe {
    double below = 0;  // its top below where its top sits seated, along the magwell axis, cm
    double side = 0;   // its top's distance from the magwell axis, cm
    double tilt = 0;   // its own axis against the magwell's, deg
};
// seat, seat_rot: where its origin sits seated and how it is turned. out:
// out of the magwell (down), any length. top: MagwellZone::top. at, rot:
// where it is held now.
TopProbe ProbeTop(const FVector& seat, const FQuat& seat_rot, const FVector& out, double top, const FVector& at,
                  const FQuat& rot);

}  // namespace holevr::vr::interact
