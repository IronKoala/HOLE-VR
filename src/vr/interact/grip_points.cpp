#include "vr/interact/grip_points.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

const char* PointKey(int id) {
    switch (id) {
        case kPointMagwell: return "magwell";
        case kPointLight: return "light";
        case kPointRailEnd: return "railend";
        default: return "free";
    }
}

const char* PointName(int id) {
    switch (id) {
        case kPointMagwell: return "in front of the magwell";
        case kPointLight: return "behind the light";
        case kPointRailEnd: return "at the end of the rail";
        default: return "where the hand grabbed";
    }
}

GripPoints MakeGripPoints(const GunParts& parts, double muzzle_dist, double hand_half, double side_gap) {
    GripPoints g;
    if (!parts.have_handguard) return g;
    const FVector lo = parts.hg_lo, hi = parts.hg_hi;
    if (!(hi.X - lo.X > 3.0 && hi.Y >= lo.Y && hi.Z >= lo.Z)) return g;
    const double cy = 0.5 * (lo.Y + hi.Y), cz = 0.5 * (lo.Z + hi.Z), hw = 0.5 * (hi.Y - lo.Y);
    // The hand against the handguard's left side, level with its middle.
    const double y = cy - hw - side_gap, z = cz;

    double front = parts.have_rails ? parts.rail_front : hi.X;
    front = std::min(front, muzzle_dist - 2.0);
    double rear = lo.X;
    if (parts.have_magwell) rear = std::max(rear, parts.magwell_front);
    const double x_mag = rear + hand_half;
    const double x_rail = front - hand_half;

    if (x_rail - x_mag < 3.0) {
        // A short handguard: one place for the hand, in its middle.
        g.have[kPointMagwell] = true;
        g.p[kPointMagwell] = FVector{0.5 * (rear + front), y, z};
    } else {
        g.have[kPointMagwell] = g.have[kPointRailEnd] = true;
        g.p[kPointMagwell] = FVector{x_mag, y, z};
        g.p[kPointRailEnd] = FVector{x_rail, y, z};
        // Behind the light, if there is room for a hand between it and the
        // other two points; otherwise halfway, if there is room for that.
        const double x_light = parts.light_rear - hand_half;
        if (parts.have_light && x_light >= x_mag + hand_half && x_light <= x_rail - hand_half) {
            g.have[kPointLight] = true;
            g.light_found = true;
            g.p[kPointLight] = FVector{x_light, y, z};
        } else if (x_rail - x_mag >= 4.0 * hand_half) {
            g.have[kPointLight] = true;
            g.p[kPointLight] = FVector{0.5 * (x_mag + x_rail), y, z};
        }
    }
    g.zone_lo = FVector{rear - 2.0 * hand_half, cy - hw - 10.0, lo.Z - 12.0};
    g.zone_hi = FVector{front + 4.0, cy + hw + 10.0, hi.Z + 8.0};
    g.ok = true;
    return g;
}

int NearestPoint(const GripPoints& g, const FVector& grab) {
    if (!g.ok) return -1;
    if (grab.X < g.zone_lo.X || grab.X > g.zone_hi.X || grab.Y < g.zone_lo.Y || grab.Y > g.zone_hi.Y ||
        grab.Z < g.zone_lo.Z || grab.Z > g.zone_hi.Z)
        return -1;
    int best = -1;
    double best_d = 1e9;
    for (int i = 0; i < kPointCount; ++i) {
        if (!g.have[i]) continue;
        const double d = std::fabs(grab.X - g.p[i].X);
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

bool LongSqueeze::Update(float value, double dt, float release, double grace) {
    lowest = std::min(lowest, value);
    if (value >= release) {
        below_for = 0;
        return true;
    }
    below_for += dt;
    return below_for < grace;
}

}  // namespace holevr::vr::interact
