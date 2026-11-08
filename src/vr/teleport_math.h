#pragma once
// Teleport logic with no game or Windows calls, so it can be unit tested:
// the aiming arc, the stick gesture and the landing check.
#include <vector>

#include "vr/math.h"

namespace holevr::vr::teleport {

// The arc: a throw from `start` along `dir` (normalised inside) at `speed`
// cm/s, falling at `gravity` cm/s^2, sampled every `step_s` seconds.
// Returns `count` points, the first being `start`.
std::vector<FVector> ArcPoints(const FVector& start, const FVector& dir, double speed, double gravity,
                               double step_s, int count);

// The stick gesture. Push the stick forward to aim, let it go to jump there,
// pull it back while aiming to cancel. After a cancel nothing happens until
// the stick is back in the middle.
enum class Gesture { kIdle, kAiming, kGo, kCancel };
class StickGesture {
public:
    Gesture Update(double x, double y);
    bool Aiming() const { return aiming_; }
    void Reset() { aiming_ = false, wait_center_ = false; }

private:
    bool aiming_ = false;
    bool wait_center_ = false;
};

// Is a hit a place you can stand? Floor-like (normal_z, the up part of the
// surface normal, at least 0.7 = up to about 45 degrees of slope), not more
// than max_rise_cm above your feet and at least 30 cm away.
struct Landing {
    bool ok = false;
    const char* why = "";  // when not ok: "too steep", "too high", "too close"
};
Landing CheckLanding(const FVector& feet, const FVector& hit, double normal_z, double max_rise_cm);

// Where the character's capsule goes so the eyes (which may be off its
// centre, head_offset horizontal) land on `hit`.
FVector CapsuleDestination(const FVector& hit, const FVector& head_offset, double capsule_half_height_cm);

}  // namespace holevr::vr::teleport
