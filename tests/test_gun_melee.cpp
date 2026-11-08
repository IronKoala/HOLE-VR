#include "vr/gun_melee_math.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    // Strike points: pistol hammer-fist only, long guns butt only.
    StrikePoint p[kMaxPoints];
    int n = StrikePoints(GunKind::kPistol, 18.0, 6.0, false, {}, p);
    Check(n == 1 && std::string(p[0].name) == "grip base", "pistol hits with the grip base only");
    Check(p[0].local.Z < -5 && p[0].local.X < 0, "pistol grip base under and behind the grip point");
    n = StrikePoints(GunKind::kLongGun, 50.0, 7.0, true, {-31.0, 0, 4.0}, p);
    Check(n == 1 && std::string(p[0].name) == "butt", "long gun hits with the butt only");
    Check(Near(p[0].local, FVector{-31.0, 0, 4.0}), "long gun uses its measured butt");
    n = StrikePoints(GunKind::kLongGun, 50.0, 7.0, true, {-3.0, 0, 4.0}, p);
    Check(Near(p[0].local, FVector{-28.0, 0, 4.0}), "an implausible stock falls back to an AR-15's butt");
    n = StrikePoints(GunKind::kLongGun, 50.0, 1000.0, false, {}, p);
    Check(std::isfinite(p[0].local.Z) && std::fabs(p[0].local.Z) < 20, "bad measurements fall back to sizes");

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

    // Held at a target by the wall check: the hand's own path still reads as
    // the swing. Last frame the muzzle was 2 cm short of a mannequin; this
    // frame the hand took it 5.5 cm on (4 m/s) but the drawn gun was held 1 cm
    // in front of the surface, 3.5 cm back.
    {
        const FVector before{98, 0, 0}, drawn{99, 0, 0}, push{-4.5, 0, 0};
        Check(Speed(before, drawn, 0.0139) < 1.0, "the drawn gun alone would read as a touch");
        Check(Near(Speed(before, HandGrip(drawn, push), 0.0139), 5.5 / 0.0139 / 100.0),
              "the hand's path reads as the real swing");
        Check(Near(HandGrip(drawn, FVector{0, 0, 80}), drawn) && Near(HandGrip(drawn, FVector{NAN, 0, 0}), drawn),
              "an unbelievable push is ignored");
    }

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
    Check(Near(Damage(t, 200.0, 1.0, 1.0), 30.0), "full pistol hammer-fist: 15% of max HP");
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

    // Butt-strike stance: main hand by the head, off hand high, not aiming.
    {
        StanceTuning st;
        auto in = [](FVector main, FVector off, FVector barrel, double now) {
            StanceInput i;
            i.valid = true;
            i.eyes = {0, 0, 160};
            i.view = {1, 0, 0};
            i.main = main;
            i.off = off;
            i.barrel = barrel;
            i.now = now;
            return i;
        };
        // Pulled back by the head, rifle near upright, butt forward-down.
        const FVector main{5, 15, 145}, off{-5, 10, 185}, up_back{-0.3, -0.1, 1.0};
        Stance a;
        Check(a.Update(in(main, off, up_back, 0.0), st), "main hand by the head, off hand high: the stance");
        // Shouldered and aiming up at 45 degrees: off hand high too, but you look along the barrel.
        Stance b;
        StanceInput aim = in({8, 12, 148}, {35, 8, 175}, {1, 0, 1}, 0.0);
        aim.view = {1, 0, 1};
        Check(!b.Update(aim, st), "aiming upwards is not the stance");
        // Hip or low ready: main hand far from the head.
        Check(!b.Update(in({30, 20, 100}, {60, 10, 120}, {0.3, 0, 1}, 0.1), st), "main hand low: no stance");
        // Off hand below the main hand (rifle upside down near the head).
        Check(!b.Update(in(main, {0, 10, 120}, {0, 0, -1}, 0.2), st), "off hand low: no stance");
        // The strike: the main hand drives forward, away from the head. Kept
        // for hold_s, then off.
        Check(a.Update(in({25, 15, 140}, off, up_back, 0.1), st), "a little out of it: still on (slack)");
        Check(a.Update(in({60, 15, 130}, {40, 10, 170}, {-0.6, 0, 0.8}, 0.2), st), "the strike keeps the pose");
        Check(a.Update(in({70, 15, 125}, {50, 10, 160}, {-0.6, 0, 0.8}, 0.6), st), "still within hold_s");
        Check(!a.Update(in({70, 15, 125}, {50, 10, 160}, {-0.6, 0, 0.8}, 0.75), st), "after hold_s: off");
        // Lost tracking is not the stance; a clock going back does not stick it on.
        Stance c;
        Check(!c.Update(StanceInput{}, st), "no tracking: no stance");
        c.Update(in(main, off, up_back, 5.0), st);
        Check(c.Update(in({70, 15, 125}, {50, 10, 160}, {-0.6, 0, 0.8}, 5.1), st), "leaving starts the hold");
        Check(c.Update(in({70, 15, 125}, {50, 10, 160}, {-0.6, 0, 0.8}, 1.0), st), "clock back: hold restarts");
        Check(!c.Update(in({70, 15, 125}, {50, 10, 160}, {-0.6, 0, 0.8}, 1.6), st), "then ends");
    }

    // Picking the hit from a multi sweep (nearest first; overlaps, then the
    // one thing that stopped it).
    {
        auto T = [](TouchKind k, bool blocking, double dist) {
            Touch t;
            t.kind = k;
            t.blocking = blocking;
            t.dist = dist;
            return t;
        };
        double wall = 1e9;
        // The hub's mannequin only overlaps the game's hit channel, then the floor stops the ball.
        Touch dummy[] = {T(TouchKind::kTarget, false, 2.0), T(TouchKind::kOther, true, 30.0)};
        Check(PickTouch(dummy, 2, 1e9, 5.0, &wall) == 0, "a target the ball passes through is a hit");
        // A wall first: the target behind it is not hit, and the wall is reported.
        Touch behind[] = {T(TouchKind::kOther, true, 4.0), T(TouchKind::kTarget, false, 9.0)};
        wall = 1e9;
        Check(PickTouch(behind, 2, 1e9, 5.0, &wall) == -1 && Near(wall, 4.0), "a wall in front stops the hit");
        // A trigger volume (overlap, not blocking) is neither a hit nor a wall.
        Touch trigger[] = {T(TouchKind::kOther, false, 1.0), T(TouchKind::kTarget, true, 6.0)};
        wall = 1e9;
        Check(PickTouch(trigger, 2, 1e9, 5.0, &wall) == 1 && wall > 1e8, "a trigger volume is passed through");
        // An enemy's capsule is passed through: its body part behind it is the hit.
        Touch enemy[] = {T(TouchKind::kPassThrough, false, 0.0), T(TouchKind::kTarget, true, 8.0)};
        Check(PickTouch(enemy, 2, 1e9, 5.0, nullptr) == 1, "an enemy capsule does not hide its body");
        // A wall another check found (the game's hit channel) still counts.
        Touch seen[] = {T(TouchKind::kTarget, true, 12.0)};
        Check(PickTouch(seen, 1, 3.0, 5.0, nullptr) == -1, "a target beyond the hit channel's wall is not hit");
        Check(PickTouch(seen, 1, 9.0, 5.0, nullptr) == 0, "within the ball's radius of that wall it is");
        Check(PickTouch(seen, 0, 1e9, 5.0, nullptr) == -1, "nothing met: no hit");
        // Speeds: a believable swing is under the jump limit, the logged spikes are not.
        Check(4.5 < kMaxSwingSpeed && 58.5 > kMaxSwingSpeed, "a grab's 58 m/s spike is a jump, a swing is not");
    }

    std::printf("gun melee: %d checks passed\n", checks);
    return 0;
}
