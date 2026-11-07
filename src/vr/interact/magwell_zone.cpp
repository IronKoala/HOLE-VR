#include "vr/interact/magwell_zone.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {
namespace {
double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
}  // namespace

MagwellZone MeasureMagwell(const std::vector<FVector>& mag_corners, const FVector& seat, const FVector& in,
                           const std::vector<PartBox>& parts) {
    MagwellZone z;
    const double len = std::sqrt(Dot(in, in));
    if (mag_corners.empty() || len < 1e-6) return z;
    const FVector u = in * (1.0 / len);
    // The magwell runs up from below: a part's bottom is where the axis meets
    // it, so the axis must climb in the gun frame.
    if (u.Z < 0.5) return z;
    FVector lo = mag_corners[0], hi = mag_corners[0];
    double top = -1e9, bottom = 1e9;
    for (const FVector& c : mag_corners) {
        lo = {std::min(lo.X, c.X), std::min(lo.Y, c.Y), std::min(lo.Z, c.Z)};
        hi = {std::max(hi.X, c.X), std::max(hi.Y, c.Y), std::max(hi.Z, c.Z)};
        const double s = Dot(c - seat, u);
        top = std::max(top, s);
        bottom = std::min(bottom, s);
    }
    if (top - bottom < 2.0) return z;
    const FVector centre = (lo + hi) * 0.5;
    // The opening: the lowest bottom of a part the magazine's axis runs up
    // into, among parts beside the seated magazine (their height overlaps
    // it). A part ahead of or behind the magwell (handguard, pistol grip of
    // an AR, trigger guard) is not on the axis at its own bottom.
    double opening = 1e9;
    std::string part;
    constexpr double kEdge = 0.3;
    for (const PartBox& p : parts) {
        if (p.lo.Z >= hi.Z - 1.0 || p.hi.Z <= lo.Z + 1.0) continue;
        const double k = (p.lo.Z - centre.Z) / u.Z;
        const FVector q = centre + u * k;
        if (q.X < p.lo.X - kEdge || q.X > p.hi.X + kEdge || q.Y < p.lo.Y - kEdge || q.Y > p.hi.Y + kEdge) continue;
        const double s = Dot(q - seat, u);
        if (s < opening) {
            opening = s;
            part = p.name;
        }
    }
    if (part.empty()) return z;
    // A part hanging well below the seated magazine is not its magwell (a
    // flush magazine sits at most a little above the opening).
    opening = std::max(opening, bottom - 0.5);
    const double insert = top - opening;
    if (insert < 1.5 || insert > 30.0) return z;
    z.ok = true;
    z.top = top;
    z.bottom = bottom;
    z.opening = opening;
    z.insert = insert;
    z.part = part;
    return z;
}

double LineUpBelow(const MagwellZone& z, double out_cm, double fallback) {
    if (!z.ok) return fallback;
    return z.insert + std::max(0.0, out_cm);
}

double SeatLast(const MagwellZone& z, double last) {
    last = std::max(0.0, last);
    if (!z.ok) return last;
    return std::min(last, z.insert * 0.5);
}

double InsertProgress(double below, double line_up, double last) {
    const double span = line_up - last;
    if (span <= 1e-6) return below <= last ? 1.0 : 0.0;
    return std::clamp((line_up - below) / span, 0.0, 1.0);
}

TopProbe ProbeTop(const FVector& seat, const FQuat& seat_rot, const FVector& out, double top, const FVector& at,
                  const FQuat& rot) {
    TopProbe t;
    const double len = std::sqrt(Dot(out, out));
    if (len < 1e-6) return t;
    const FVector u = out * (1.0 / len);
    const FQuat inv = seat_rot.Inverse();
    // Its top (on the magwell axis when seated) and its axis, in its own frame.
    const FVector top_local = inv.Rotate(u * -top);
    const FVector axis_local = inv.Rotate(u * -1.0);
    const FVector d = (at + rot.Rotate(top_local)) - (seat - u * top);
    t.below = Dot(d, u);
    const FVector side = d - u * t.below;
    t.side = std::sqrt(Dot(side, side));
    const double c = std::clamp(Dot(rot.Rotate(axis_local), u * -1.0), -1.0, 1.0);
    t.tilt = std::acos(c) * 180.0 / 3.14159265358979;
    return t;
}

}  // namespace holevr::vr::interact
