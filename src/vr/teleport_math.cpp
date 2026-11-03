#include "vr/teleport_math.h"

#include <cmath>

namespace holevr::vr::teleport {

std::vector<FVector> ArcPoints(const FVector& start, const FVector& dir, double speed, double gravity,
                               double step_s, int count) {
    std::vector<FVector> out;
    if (count <= 0) return out;
    double len = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
    FVector d = len > 1e-9 ? dir * (1.0 / len) : FVector{1, 0, 0};
    FVector v = d * speed;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        double t = i * step_s;
        out.push_back(FVector{start.X + v.X * t, start.Y + v.Y * t, start.Z + v.Z * t - 0.5 * gravity * t * t});
    }
    return out;
}

Gesture StickGesture::Update(double x, double y) {
    const double mag = std::sqrt(x * x + y * y);
    if (wait_center_) {
        if (mag < 0.3) wait_center_ = false;
        return Gesture::kIdle;
    }
    if (!aiming_) {
        // Forward, not sideways or back.
        if (mag > 0.7 && y > 0.5) {
            aiming_ = true;
            return Gesture::kAiming;
        }
        return Gesture::kIdle;
    }
    if (y < -0.6) {
        aiming_ = false;
        wait_center_ = true;
        return Gesture::kCancel;
    }
    if (mag < 0.3) {
        aiming_ = false;
        return Gesture::kGo;
    }
    return Gesture::kAiming;
}

Landing CheckLanding(const FVector& feet, const FVector& hit, double normal_z, double max_rise_cm) {
    Landing l;
    if (!(normal_z >= 0.7)) {
        l.why = "too steep";
        return l;
    }
    if (hit.Z - feet.Z > max_rise_cm) {
        l.why = "too high";
        return l;
    }
    double dx = hit.X - feet.X, dy = hit.Y - feet.Y;
    if (dx * dx + dy * dy < 30.0 * 30.0 && std::fabs(hit.Z - feet.Z) < 50.0) {
        l.why = "too close";
        return l;
    }
    l.ok = true;
    return l;
}

FVector CapsuleDestination(const FVector& hit, const FVector& head_offset, double capsule_half_height_cm) {
    // A little above the floor so the capsule never starts inside it.
    return FVector{hit.X - head_offset.X, hit.Y - head_offset.Y, hit.Z + capsule_half_height_cm + 2.0};
}

}  // namespace holevr::vr::teleport
