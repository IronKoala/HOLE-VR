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

void Stats::Add(const FVector& actual, const Leads& l, int used) { Add(actual, l, used, 0.0); }

void Stats::Add(const FVector& actual, const Leads& l, int used, double yaw_deg) {
    const double none = Length(actual);
    // Count frames where the camera moved, or a prediction said it did.
    bool moving = none > 0.5;
    for (int k = 0; k < kKinds; ++k)
        if (l.valid[k] && Length(l.move[k]) > 0.5) moving = true;
    if (!moving) return;
    ++frames;
    none_sum += none;
    if (none > none_max) none_max = none;
    const FVector miss = used >= 0 && used < kKinds ? actual - l.move[used] : actual;
    const double u = Length(miss);
    used_sum += u;
    if (u > used_max) used_max = u;
    const double yaw = yaw_deg * kDegToRad, c = std::cos(yaw), s = std::sin(yaw);
    const FVector local{miss.X * c + miss.Y * s, -miss.X * s + miss.Y * c, miss.Z};
    axis_sum[0] += std::fabs(local.X);
    axis_sum[1] += std::fabs(local.Y);
    axis_sum[2] += std::fabs(local.Z);
    // Keep the worst few, largest first.
    if (u > 1.0) {
        int at = worst_count < kWorst ? worst_count : kWorst;
        while (at > 0 && worst[at - 1].cm < u) --at;
        if (at < kWorst) {
            for (int i = (worst_count < kWorst ? worst_count : kWorst - 1); i > at; --i) worst[i] = worst[i - 1];
            worst[at] = Worst{u, local, actual, used, l};
            if (worst_count < kWorst) ++worst_count;
        }
    }
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

double AngleDeg(const FQuat& q) {
    const double w = std::fmin(1.0, std::fabs(q.W) / std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W));
    return 2.0 * std::acos(w) * kRadToDeg;
}

void GunShake::Add(bool valid, const FVector& at, const FQuat& rot, bool moving) {
    if (!valid) {
        have_ = 0;
        return;
    }
    if (have_ >= 2) {
        const FVector acc = at - p1_ * 2.0 + p2_;
        const FQuat a = rot * q1_.Inverse(), b = q1_ * q2_.Inverse();
        const double cm = Length(acc), deg = AngleDeg(a * b.Inverse());
        // Bigger than this is a draw, holster or teleport, not shake.
        if (cm < 30.0 && deg < 30.0) {
            const int m = moving ? 1 : 0;
            ++n[m];
            cm_sum[m] += cm;
            deg_sum[m] += deg;
            if (cm > cm_max[m]) cm_max[m] = cm;
            if (deg > deg_max[m]) deg_max[m] = deg;
        }
    }
    p2_ = p1_;
    q2_ = q1_;
    p1_ = at;
    q1_ = rot;
    if (have_ < 2) ++have_;
}

void GunShake::AddMovedAfter(double cm) {
    ++checked_frames;
    if (cm > 0.05) ++moved_frames;
    if (cm > moved_max) moved_max = cm;
}

void GunShake::AddPlacement(bool valid, const FVector& at, const FQuat& rot, bool moving) {
    if (!valid) {
        place_have_ = false;
        return;
    }
    if (place_have_) {
        const double cm = Length(at - place_p_), deg = AngleDeg(rot * place_q_.Inverse());
        // Bigger than this is a re-grip, two hands taking over or a teleport.
        if (cm < 20.0 && deg < 30.0) {
            const int m = moving ? 1 : 0;
            ++place_n[m];
            place_cm_sum[m] += cm;
            place_deg_sum[m] += deg;
            if (cm > place_cm_max[m]) place_cm_max[m] = cm;
            if (deg > place_deg_max[m]) place_deg_max[m] = deg;
        }
    }
    place_p_ = at;
    place_q_ = rot;
    place_have_ = true;
}

void GunShake::ResetTotals() {
    for (int m = 0; m < 2; ++m) {
        place_n[m] = 0;
        place_cm_sum[m] = place_cm_max[m] = place_deg_sum[m] = place_deg_max[m] = 0;
    }
    for (int m = 0; m < 2; ++m) n[m] = 0, cm_sum[m] = cm_max[m] = deg_sum[m] = deg_max[m] = 0;
    moved_frames = checked_frames = 0;
    moved_max = 0;
}

}  // namespace holevr::vr::view_lead
