#include "vr/comfort_math.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::comfort {

double ClearHalfAngleDeg(double strength) {
    strength = std::clamp(strength, 0.0, 1.0);
    return 62.0 - 34.0 * strength;
}

namespace {
double SmoothStep(double a, double b, double x) {
    double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
uint32_t Black(double alpha) {
    uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    return a << 24;  // premultiplied black: colour stays 0
}
}  // namespace

std::vector<uint32_t> TexturePixels() {
    std::vector<uint32_t> px(static_cast<size_t>(kTexW) * kTexH, 0u);
    for (int t = 0; t < kVignetteLevels; ++t) {
        const double strength = (t + 1.0) / kVignetteLevels;
        // Black before the quad's edge (71.6 deg) at every strength.
        const double inner = ClearHalfAngleDeg(strength), outer = std::min(inner + kRampDeg, 68.0);
        const int ox = (t % kTilesPerRow) * kTile, oy = (t / kTilesPerRow) * kTile;
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                double u = (x + 0.5) / kTile * 2.0 - 1.0, v = (y + 0.5) / kTile * 2.0 - 1.0;
                double r = std::sqrt(u * u + v * v);
                double deg = std::atan(kQuadHalfM * r / kQuadDistM) * kRadToDeg;
                px[static_cast<size_t>(oy + y) * kTexW + ox + x] = Black(SmoothStep(inner, outer, deg));
            }
        }
    }
    const int fy = kTile * (kVignetteLevels / kTilesPerRow);
    for (int k = 0; k < kFadeLevels; ++k) {
        const uint32_t c = Black((k + 1.0) / kFadeLevels);
        for (int y = 0; y < kFadeBlock; ++y)
            for (int x = 0; x < kFadeBlock; ++x)
                px[static_cast<size_t>(fy + y) * kTexW + k * kFadeBlock + x] = c;
    }
    return px;
}

int VignetteLevel(double strength) {
    if (!(strength >= 0.02)) return 0;
    return std::clamp(static_cast<int>(std::ceil(strength * kVignetteLevels)), 1, kVignetteLevels);
}

Rect VignetteTile(int level) {
    int t = std::clamp(level, 1, kVignetteLevels) - 1;
    return Rect{(t % kTilesPerRow) * kTile, (t / kTilesPerRow) * kTile, kTile, kTile};
}

int FadeLevel(double opacity) {
    if (!(opacity >= 0.03)) return 0;
    return std::clamp(static_cast<int>(std::ceil(opacity * kFadeLevels)), 1, kFadeLevels);
}

Rect FadeBlock(int level) {
    int k = std::clamp(level, 1, kFadeLevels) - 1;
    constexpr int inset = 8;
    return Rect{k * kFadeBlock + inset, kTile * (kVignetteLevels / kTilesPerRow) + inset, kFadeBlock - 2 * inset,
                kFadeBlock - 2 * inset};
}

double Smoother::Step(double target, double dt, double rise_s, double fall_s) {
    double tau = target > value ? rise_s : fall_s;
    if (tau <= 1e-6 || dt <= 0) {
        if (dt > 0 || tau <= 1e-6) value = target;
        return value;
    }
    value += (target - value) * (1.0 - std::exp(-dt / tau));
    if (std::fabs(target - value) < 1e-4) value = target;
    return value;
}

// ---- physical crouch and jump ----
void PhysicalBody::Configure(bool crouch, double crouch_drop_cm, bool jump) {
    crouch_on_ = crouch;
    jump_on_ = jump;
    drop_m_ = std::clamp(crouch_drop_cm, 15.0, 90.0) / 100.0;
    if (!crouch_on_) crouched_ = false;
}

void PhysicalBody::SetStanding(double head_m) {
    if (!(head_m > 0.3 && head_m < 2.5)) return;
    standing_ = head_m;
    measured_ = true;
}

void PhysicalBody::Reset() {
    bool measured = measured_;
    double standing = standing_;
    *this = PhysicalBody{};
    if (measured) {
        measured_ = true;
        standing_ = standing;
    }
}

BodyEvent PhysicalBody::Update(double head_m, double dt) {
    if (!(dt > 0) || dt > 0.5 || !std::isfinite(head_m)) return BodyEvent::kNone;
    if (!have_) {
        have_ = true;
        last_ = head_m;
        if (!measured_) standing_ = head_m;
        vel_ = 0;
        warmup_ = 0;
        return BodyEvent::kNone;
    }
    double raw_vel = (head_m - last_) / dt;
    last_ = head_m;
    vel_ += (raw_vel - vel_) * (1.0 - std::exp(-dt / 0.05));
    // Standing height: the highest the head settles (not while moving fast,
    // so a hop or a tracking jump never raises it). Spikes above 2.3 m are
    // tracking errors, not a player.
    if (!measured_ && head_m < 2.3 && std::fabs(vel_) < 0.3 && head_m > standing_) standing_ = head_m;
    if (warmup_ < 1.5) {
        warmup_ += dt;
        return BodyEvent::kNone;
    }
    if (jump_cooldown_ > 0) jump_cooldown_ -= dt;
    if (jump_on_ && !crouched_ && jump_cooldown_ <= 0 && vel_ > 1.1 && head_m > standing_ + 0.03) {
        jump_cooldown_ = 1.0;
        return BodyEvent::kJump;
    }
    if (!crouch_on_) return BodyEvent::kNone;
    double drop = standing_ - head_m;
    if (!crouched_ && drop > drop_m_) {
        crouched_ = true;
        return BodyEvent::kCrouch;
    }
    if (crouched_ && drop < drop_m_ - 0.10) {
        crouched_ = false;
        return BodyEvent::kStand;
    }
    return BodyEvent::kNone;
}

// ---- aim smoothing ----
double AngleDeg(const FQuat& a, const FQuat& b) {
    double d = std::fabs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
    return 2.0 * std::acos(std::min(1.0, d)) * kRadToDeg;
}

namespace {
FQuat Nlerp(const FQuat& a, FQuat b, double t) {
    if (a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W < 0) b = FQuat{-b.X, -b.Y, -b.Z, -b.W};
    FQuat r{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t};
    double l = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W);
    return l > 1e-9 ? FQuat{r.X / l, r.Y / l, r.Z / l, r.W / l} : b;
}
}  // namespace

FQuat AimSmoother::Step(const FQuat& target, double dt, double strength) {
    if (strength <= 0.0 || !have_ || dt <= 0 || dt > 0.25) {
        have_ = true;
        q_ = target;
        return q_;
    }
    const double off = AngleDeg(q_, target);
    if (off > 20.0) {  // a big move (or a teleport): never trail behind it
        q_ = target;
        return q_;
    }
    // How fast the hand turns, from where it is to where it was smoothed to.
    const double speed = off / dt;              // deg/s
    const double slow = std::max(0.0, 1.0 - speed / 60.0);
    const double tau = std::clamp(strength, 0.0, 1.0) * 0.12 * slow * slow;
    const double alpha = tau > 1e-5 ? 1.0 - std::exp(-dt / tau) : 1.0;
    q_ = Nlerp(q_, target, alpha);
    return q_;
}

GripView ToggleGrip(bool toggle, bool holding, bool down, bool pressed, bool released) {
    if (!toggle || !holding) return GripView{down, pressed, released};
    // Holding in toggle mode: the grip counts as held until the next squeeze,
    // which is the let-go. Letting the button up does nothing.
    if (pressed) return GripView{false, false, true};
    return GripView{true, false, false};
}

double CameraHeight::Update(double rel_cm, bool crouched, bool crouch_press, bool still, bool real_mode, double dt) {
    if (!(dt > 0) || dt > 0.5 || !std::isfinite(rel_cm)) return extra_;
    if (crouch_press) {
        ++presses_;
        standing_t_ = 0;  // the camera takes a moment to start moving
    }
    // Standing height: the highest value the camera holds still at for 1 s
    // while not crouched. Animations that lift it for a moment never count.
    if (!crouched && rel_cm > 60.0 && rel_cm < 260.0) {
        if (std::fabs(rel_cm - stable_ref_) < 1.0) {
            stable_t_ += dt;
        } else {
            stable_ref_ = rel_cm;
            stable_t_ = 0;
        }
        // Later it may only creep up a little: a camera held 14 cm higher for
        // a second (2026-11-03 run 2, after the title: 165 against 151) is
        // not the character standing taller.
        if (stable_t_ >= 1.0 && (!have_ || (stable_ref_ > stand_ && stable_ref_ <= first_ + 4.0))) {
            if (!have_) first_ = stable_ref_;
            stand_ = stable_ref_;
            have_ = true;
        }
    } else {
        stable_t_ = 0;
    }
    drop_ = have_ ? std::max(0.0, stand_ - rel_cm) : 0.0;
    // Back at standing height for a moment: earlier crouch presses are done with.
    if (drop_ < 5.0) {
        standing_t_ += dt;
        if (standing_t_ >= 0.6) presses_ = 0;
    } else {
        standing_t_ = 0;
    }
    const bool explained = crouched || (presses_ % 2) == 1;
    double want = 0;
    if (real_mode) {
        want = drop_;
        holding_ = false;
        low_t_ = 0;
    } else {
        // Starts only after 2 s standing still with the camera low and no
        // crouch; once started it lasts while the camera stays low, moving
        // or not. Sprinting dips (2026-11-03 run: 11-26 cm every sprint) never
        // start it.
        const bool low = drop_ > 8.0 && !explained;
        low_t_ = low && still ? low_t_ + dt : 0.0;
        holding_ = low && (holding_ || low_t_ >= 2.0);
        want = holding_ ? drop_ : 0.0;
    }
    want = std::min(want, 80.0);
    extra_ += (want - extra_) * (1.0 - std::exp(-6.0 * dt));
    if (want == 0 && extra_ < 0.05) extra_ = 0;
    return extra_;
}

double WallFade(const FVector& eye, const FVector& impact, const FVector& normal, double start, double full,
                double* gap) {
    const double len = std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
    if (len < 0.5 || start <= full) {
        if (gap) *gap = start;
        return 0.0;
    }
    const FVector d = eye - impact;
    const double h = (d.X * normal.X + d.Y * normal.Y + d.Z * normal.Z) / len;
    if (gap) *gap = h;
    return std::clamp((start - h) / (start - full), 0.0, 1.0);
}

}  // namespace holevr::vr::comfort
