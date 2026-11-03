#include "vr/view_lead_math.h"

#include <cmath>

namespace holevr::vr::view_lead {

const char* const kKindNames[kKinds] = {"camera", "body", "capsule"};

double Length(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

Leads Predict(const Anchors& then, const Anchors& now, const FVector& camera_then, double max_cm) {
    Leads l;
    if (!then.who || then.who != now.who) return l;
    if (then.camera && now.camera) {
        l.move[kCamera] = now.camera_at - then.camera_at;
        l.valid[kCamera] = true;
    }
    if (then.mesh && now.mesh) {
        // The camera where it was against the body then, carried along with
        // the body (its move and its turn) to now.
        const FVector rel = then.mesh_rot.Inverse().Rotate(camera_then - then.mesh_at);
        l.move[kMesh] = now.mesh_at + now.mesh_rot.Rotate(rel) - camera_then;
        l.valid[kMesh] = true;
    }
    if (then.capsule && now.capsule) {
        l.move[kCapsule] = now.capsule_at - then.capsule_at;
        l.valid[kCapsule] = true;
    }
    for (int k = 0; k < kKinds; ++k) {
        if (l.valid[k] && !(Length(l.move[k]) <= max_cm)) {
            l.valid[k] = false;
            l.move[k] = FVector{};
        }
    }
    return l;
}

int Pick(const Leads& l, int choice) {
    if (choice == 0) return -1;
    if (choice >= 1 && choice <= kKinds) return l.valid[choice - 1] ? choice - 1 : -1;
    for (int k = 0; k < kKinds; ++k)
        if (l.valid[k]) return k;
    return -1;
}

void Stats::Add(const FVector& actual, const Leads& l, int used) {
    const double none = Length(actual);
    // Count frames where the camera moved, or a prediction said it did.
    bool moving = none > 0.5;
    for (int k = 0; k < kKinds; ++k)
        if (l.valid[k] && Length(l.move[k]) > 0.5) moving = true;
    if (!moving) return;
    ++frames;
    none_sum += none;
    if (none > none_max) none_max = none;
    const double u = used >= 0 && used < kKinds ? Length(actual - l.move[used]) : none;
    used_sum += u;
    if (u > used_max) used_max = u;
    for (int k = 0; k < kKinds; ++k) {
        if (!l.valid[k]) continue;
        ++n[k];
        sum[k] += Length(actual - l.move[k]);
    }
}

void Stats::AddTurn(double degrees) {
    const double a = std::fabs(degrees);
    if (!(a > 0.05) || a > 90.0) return;  // still, or a recentre / respawn
    ++turn_frames;
    turn_sum += a;
    if (a > turn_max) turn_max = a;
}

}  // namespace holevr::vr::view_lead
