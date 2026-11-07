#include "vr/gun_melee_math.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace holevr::vr;
using namespace holevr::vr::gun_melee;
namespace {
int checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
bool Near(FVector a, FVector b, double eps = 1e-6) { return Length(a - b) < eps; }
}  // namespace

int main() {
    // Strike points.
    StrikePoint p[kMaxPoints];
    int n = StrikePoints(GunKind::kPistol, 18.0, 6.0, false, {}, p);
    Check(n == 3, "pistol has three strike points");
    Check(p[0].local.X > p[1].local.X && p[1].local.X > 0, "pistol muzzle ahead of the slide's middle");
    Check(p[2].local.Z < -5 && p[2].local.X < 0, "pistol grip base under and behind the grip point");
    n = StrikePoints(GunKind::kLongGun, 50.0, 7.0, true, {-31.0, 0, 4.0}, p);
    Check(n == 3 && Near(p[0].local, FVector{-31.0, 0, 4.0}), "long gun uses its measured butt");
    Check(Near(p[1].local, FVector{50.0, 0, 7.0}), "long gun muzzle at the bore's end");
    n = StrikePoints(GunKind::kLongGun, 50.0, 7.0, true, {-3.0, 0, 4.0}, p);
    Check(Near(p[0].local, FVector{-28.0, 0, 4.0}), "an implausible stock falls back to an AR-15's butt");
    n = StrikePoints(GunKind::kPistol, NAN, 1000.0, false, {}, p);
    Check(std::isfinite(p[0].local.X) && std::fabs(p[0].local.Z) < 20, "bad measurements fall back to sizes");

    // Tracking-space point: the world moved (walk, stick turn, recentre),
    // the controller and gun did not move for real: same tracking point.
    const FVector local_ctrl_t{30, 10, 120};
    const FQuat ctrl_rot_t = ToQuat({10, 20, 30});
    const FVector offset_in_ctrl{15, 2, 5};
    auto world_of = [&](const FVector& origin, const FQuat& turn, double scale, FVector* ctrl_w, FQuat* rot_w) {
        *ctrl_w = origin + turn.Rotate(local_ctrl_t * scale);
        *rot_w = turn * ctrl_rot_t;
        return *ctrl_w + rot_w->Rotate(offset_in_ctrl * scale);
    };
    FVector cw1, cw2;
    FQuat rw1, rw2;
    const FVector pw1 = world_of({100, 200, 0}, ToQuat({0, 0, 0}), 1.0, &cw1, &rw1);
    const FVector pw2 = world_of({-500, 900, 40}, ToQuat({0, 135, 0}), 1.0, &cw2, &rw2);
    const FVector t1 = TrackingPoint(pw1, cw1, rw1, local_ctrl_t, ctrl_rot_t, 1.0);
    const FVector t2 = TrackingPoint(pw2, cw2, rw2, local_ctrl_t, ctrl_rot_t, 1.0);
    Check(Near(t1, t2, 1e-6), "moving and turning the world is not a swing");
    Check(Near(t1, local_ctrl_t + ctrl_rot_t.Rotate(offset_in_ctrl)), "tracking point is the point on the controller");
    FVector cw3;
    FQuat rw3;
    const FVector pw3 = world_of({0, 0, 0}, ToQuat({0, 40, 0}), 1.5, &cw3, &rw3);
    Check(Near(TrackingPoint(pw3, cw3, rw3, local_ctrl_t, ctrl_rot_t, 1.5), t1, 1e-6),
          "world scale does not change the real distance");

    // Speed: a muzzle 20 cm from the wrist turned 60 degrees in one 72 Hz frame.
    Check(Near(Speed({0, 0, 0}, {5, 0, 0}, 0.0139), 5.0 / 0.0139 / 100.0), "speed in m/s");
    Check(Speed({0, 0, 0}, {5, 0, 0}, 0.0) == 0.0, "no time between samples: no speed");
    Check(Speed({0, 0, 0}, {5, 0, 0}, 0.5) == 0.0, "a long gap (paused) is not a swing");
    // Aiming steadily: 1 cm per frame is 0.7 m/s, well under a hit.
    Check(Speed({0, 0, 0}, {1, 0, 0}, 0.0139) < 2.0, "steady aiming is not a hit");

    // Swing scale.
    Check(SwingScale(1.9, 2.0, 4.5) == 0.0, "below the hit speed: nothing");
    Check(Near(SwingScale(2.0, 2.0, 4.5), 0.4), "at the hit speed: 40%");
    Check(Near(SwingScale(4.5, 2.0, 4.5), 1.0) && Near(SwingScale(9.0, 2.0, 4.5), 1.0), "full from full speed");
    Check(SwingScale(3.25, 2.0, 4.5) > 0.69 && SwingScale(3.25, 2.0, 4.5) < 0.71, "half way: 70%");
    Check(SwingScale(NAN, 2.0, 4.5) == 0.0, "NaN speed is no hit");

    // Damage and stun.
    Tuning t;
    t.damage_share = 0.15;
    t.stun_share = 0.7;
    Check(Near(Damage(t, 200.0, 1.0, 1.0), 30.0), "full pistol whip: 15% of max HP");
    Check(Near(Damage(t, 200.0, 0.5, 2.0), 30.0), "half swing to a x2 body part");
    Check(Near(Damage(t, -1.0, 1.0, 1.0), 15.0), "unknown HP counts as 100");
    Check(Damage(t, 100.0, 1.0, NAN) == 15.0, "bad multiplier counts as 1");
    Check(Near(Stun(t, 50.0, 1.0, 0.5), 35.0), "stun multiplier never below 1, like the kick");
    Check(Near(Stun(t, 50.0, 1.0, 2.0), 70.0), "stun multiplier above 1 applies");

    // Cooldown: one swing through a body is one hit.
    Cooldowns c;
    Check(c.Ready(1, 10.0, 0.45), "first hit allowed");
    c.Mark(1, 10.0);
    Check(!c.Ready(1, 10.2, 0.45), "same target in the same swing: no second hit");
    Check(c.Ready(2, 10.2, 0.45), "another target in the same swing: hit");
    Check(c.Ready(1, 10.5, 0.45), "same target after the cooldown: hit");
    for (uintptr_t k = 10; k < 30; ++k) c.Mark(k, 11.0 + static_cast<double>(k) * 0.01);
    Check(!c.Ready(29, 11.3, 0.45), "the newest of many targets is remembered");
    c.Clear();
    Check(c.Ready(29, 11.3, 0.45), "clear forgets");

    std::printf("gun melee: %d checks passed\n", checks);
    return 0;
}
