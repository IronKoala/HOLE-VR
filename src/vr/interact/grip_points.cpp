#include "vr/interact/grip_points.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

const char* PointKey(int id) {
    switch (id) {
        case kPointMag: return "mag";
        case kPointScopeRail: return "scoperail";
        case kPointLight: return "light";
        default: return "free";
    }
}

const char* PointName(int id) {
    switch (id) {
        case kPointMag: return "in front of the magazine";
        case kPointScopeRail: return "where the scope rail ends";
        case kPointLight: return "behind the light";
        default: return "where the hand grabbed";
    }
}

namespace {
double DistXZ(const FVector& a, const FVector& b) {
    const double dx = a.X - b.X, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dz * dz);
}
}  // namespace

GripPoints MakeGripPoints(const GunParts& parts, double muzzle_dist, double hand_half, double side_gap) {
    GripPoints g;
    if (!parts.have_handguard) return g;
    const FVector lo = parts.hg_lo, hi = parts.hg_hi;
    if (!(hi.X - lo.X > 3.0 && hi.Y >= lo.Y && hi.Z >= lo.Z)) return g;
    const double cy = 0.5 * (lo.Y + hi.Y), cz = 0.5 * (lo.Z + hi.Z), hw = 0.5 * (hi.Y - lo.Y);
    // On the handguard: the hand against its left side, level with its middle.
    const double y = cy - hw - side_gap, z = cz;

    double front = parts.have_rails ? parts.rail_front : hi.X;
    front = std::min(front, muzzle_dist - 2.0);
    // The handguard starts in front of the receiver.
    double rear = lo.X;
    if (parts.have_magwell) rear = std::max(rear, parts.magwell_front);
    if (front - rear < 3.0) return g;

    // In front of the magazine: the fist (its grip point a little in front
    // of the magazine's front face, at its left side), below the handguard,
    // where the owner reached for it (second AR run: X 13-17, Z -3..5 with
    // the handguard's bottom at 8.7).
    constexpr double kFistGap = 3.0, kBelowHandguard = 8.0;
    FVector mag;
    const bool mag_ok = parts.have_mag && parts.mag_hi.X > parts.mag_lo.X && parts.mag_hi.X > 0.0 &&
                        parts.mag_hi.X < front && parts.mag_lo.Z < lo.Z;
    if (mag_ok) {
        const double mz = std::min(lo.Z, std::max(parts.mag_lo.Z + 1.0, lo.Z - kBelowHandguard));
        mag = FVector{parts.mag_hi.X + kFistGap, cy - hw, mz};
        g.mag_found = true;
    } else if (parts.have_magwell) {
        // No magazine in the gun: just behind the receiver's front end, where
        // a magazine's front would be.
        mag = FVector{parts.magwell_front - 2.0, cy - hw, lo.Z - kBelowHandguard};
    } else {
        mag = FVector{rear + hand_half, y, z};
    }

    // Behind the light, or at the front of the handguard.
    FVector light{front - hand_half, y, z};
    if (parts.have_light && parts.light_rear >= rear + 2.0 * hand_half && parts.light_rear <= front + hand_half) {
        light.X = parts.light_rear - hand_half;
        g.light_found = true;
    }

    // Where the scope rail ends, on the handguard; or halfway along it.
    FVector scope{0.5 * (rear + hand_half + light.X), y, z};
    if (parts.have_scope_rail) {
        scope.X = std::clamp(parts.scope_rail_front, rear + hand_half, std::max(rear + hand_half, front - hand_half));
        g.scope_found = true;
    }

    // Kept in this order, each at least a hand's half width from the ones
    // kept before (a short handguard has room for fewer).
    const int order[kPointCount] = {kPointMag, kPointLight, kPointScopeRail};
    const FVector at[kPointCount] = {mag, scope, light};
    for (int id : order) {
        bool room = true;
        for (int k = 0; k < kPointCount; ++k)
            if (g.have[k] && DistXZ(g.p[k], at[id]) < hand_half) room = false;
        if (!room) continue;
        g.have[id] = true;
        g.p[id] = at[id];
    }
    if (!g.have[kPointScopeRail]) g.scope_found = false;
    if (!g.have[kPointLight]) g.light_found = false;

    const double zone_rear = std::min(rear, g.p[kPointMag].X) - 2.0 * hand_half;
    g.zone_lo = FVector{zone_rear, cy - hw - 10.0, std::min(lo.Z - 12.0, g.p[kPointMag].Z - 8.0)};
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
        const double d = DistXZ(grab, g.p[i]);
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
