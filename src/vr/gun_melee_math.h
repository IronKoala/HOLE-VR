#pragma once
// Gun melee (pistol hammer-fist, long-gun butt strikes): the parts of a held
// gun that can hit, how fast they move, and how hard a hit is. No engine
// access, so it can be tested on its own (tests/test_gun_melee.cpp).
//
// Speeds are measured in the headset's own tracking space: walking, stick
// turns, snap turns and recentring move the world around you but never count
// as a swing. Only your real arm moving the gun does.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "vr/math.h"

namespace holevr::vr::gun_melee {

enum class GunKind { kPistol, kLongGun };

// One part of the gun that can strike: a ball in the gun frame (X along the
// barrel, Y right, Z up, origin at the firing hand's grip point), cm.
struct StrikePoint {
    const char* name = "";
    FVector local;
    double radius = 3.0;
};
constexpr int kMaxPoints = 4;

// What can strike (owner 2026-11-08: "Set Pistol to hammer-fist only.
// AR/SG/SR to butt strikes only"):
// A pistol: the bottom of the grip only (a hammer-fist strike with the
// magazine base plate). The grip point sits about two thirds of the way up
// the grip, so the base plate is ~7 cm under it and, with the grip's ~20
// degree rake, ~2.5 cm behind it.
// A long gun (rifle, shotgun, sniper rifle): the butt only (the measured
// stock pad when there is one).
inline int StrikePoints(GunKind kind, double muzzle_dist, double bore_height, bool have_stock,
                        const FVector& stock, StrikePoint out[kMaxPoints]) {
    (void)muzzle_dist;
    if (!std::isfinite(bore_height) || std::fabs(bore_height) > 20.0) bore_height = 6.0;
    if (kind == GunKind::kPistol) {
        out[0] = {"grip base", {-2.5, 0, -7.0}, 3.5};
        return 1;
    }
    FVector butt = have_stock ? stock : FVector{-28.0, 0, bore_height - 3.0};
    if (!std::isfinite(butt.X) || butt.X > -10.0 || butt.X < -60.0) butt = FVector{-28.0, 0, bore_height - 3.0};
    out[0] = {"butt", butt, 5.0};
    return 1;
}

// Where a world point that moves with a controller is in tracking space
// (UE axes, cm of real distance). `ctrl_*_world` is the controller's pose as
// mapped into the world this frame, `ctrl_*_track` its own tracking pose
// (UE axes, cm), `world_per_real` world cm per real cm (world scale).
// Within one frame the world mapping is one rigid move plus a scale, so this
// is that point in tracking space exactly, whatever moved the world.
inline FVector TrackingPoint(const FVector& p_world, const FVector& ctrl_loc_world, const FQuat& ctrl_rot_world,
                             const FVector& ctrl_loc_track, const FQuat& ctrl_rot_track, double world_per_real) {
    if (!(world_per_real > 1e-3)) world_per_real = 1.0;
    const FVector local = ctrl_rot_world.Inverse().Rotate(p_world - ctrl_loc_world) * (1.0 / world_per_real);
    return ctrl_loc_track + ctrl_rot_track.Rotate(local);
}

inline double Length(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

// Where the hand is taking the gun's grip point: the drawn grip less what the
// wall check pushed it this frame (it holds the gun out of whatever it
// reaches, a target included). A push that is not believable is ignored.
inline FVector HandGrip(const FVector& drawn_grip, const FVector& wall_push) {
    const double len = std::sqrt(wall_push.X * wall_push.X + wall_push.Y * wall_push.Y + wall_push.Z * wall_push.Z);
    if (!std::isfinite(len) || len >= 60.0) return drawn_grip;
    return drawn_grip - wall_push;
}

// Real speed of a point in m/s from two tracking-space positions (cm) dt
// seconds apart. 0 when dt is not a believable frame time.
inline double Speed(const FVector& before_cm, const FVector& now_cm, double dt) {
    if (!(dt > 0.002) || dt > 0.1) return 0.0;
    return Length(now_cm - before_cm) / dt / 100.0;
}

// How much of a full hit a swing at `speed` m/s is: nothing below min_speed,
// 40 % at min_speed rising evenly to 100 % at full_speed and above.
inline double SwingScale(double speed, double min_speed, double full_speed) {
    if (!std::isfinite(speed) || speed < min_speed) return 0.0;
    if (!(full_speed > min_speed)) return 1.0;
    const double t = std::clamp((speed - min_speed) / (full_speed - min_speed), 0.0, 1.0);
    return 0.4 + 0.6 * t;
}

struct Tuning {
    double min_speed = 2.0;      // m/s: slower contact is a touch, not a hit
    double full_speed = 4.5;     // m/s: a full swing
    double damage_share = 0.15;  // of the target's max HP, full swing, before the game's body-part multiplier
    double stun_share = 0.7;     // of the player's kick stun, full swing
    double cooldown_s = 0.45;    // the same gun hits the same target at most this often
};

// The hit's damage: a share of the target's max HP (falls back to 100 HP
// when the target's own is unknown), times the swing, times the game's own
// multiplier for the body part and armour (the one its bullets and melee
// weapons use).
inline double Damage(const Tuning& t, double max_hp, double swing, double game_multiplier) {
    if (!std::isfinite(max_hp) || max_hp <= 0.0 || max_hp > 1e6) max_hp = 100.0;
    if (!std::isfinite(game_multiplier) || game_multiplier < 0.0) game_multiplier = 1.0;
    return std::max(0.0, t.damage_share * max_hp * swing * game_multiplier);
}

// The hit's stun: a share of the kick's stun, times the swing, times the
// game's stun multiplier for the body part (never below 1, like the kick).
inline double Stun(const Tuning& t, double kick_stun, double swing, double game_multiplier) {
    if (!std::isfinite(kick_stun) || kick_stun < 0.0) kick_stun = 0.0;
    if (!std::isfinite(game_multiplier) || game_multiplier < 1.0) game_multiplier = 1.0;
    return std::max(0.0, t.stun_share * kick_stun * swing * game_multiplier);
}

// A strike point moving faster than this (m/s) jumped (the gun snapping into
// a hand, a grip change, a lost frame): not a swing. A hard punch is about
// 10 m/s; the logs show 58 m/s spikes as a gun is taken.
constexpr double kMaxSwingSpeed = 15.0;
// After a gun comes into a hand (or changes hands) its first moments are the
// grab settling, not a swing.
constexpr double kSettleS = 0.3;

// One thing a sweep met, in the order the sweep met them (nearest first, as
// the engine returns a multi sweep: its overlaps, then the one blocking hit).
enum class TouchKind {
    kTarget,       // something the game's hit event can hit (a mannequin, a target, an enemy's body part)
    kPassThrough,  // an enemy's capsule or what it carries, the player: neither a hit nor a wall
    kOther,        // anything else (a wall, the floor, a trigger volume)
};
struct Touch {
    TouchKind kind = TouchKind::kOther;
    bool blocking = false;  // the sweep stopped here
    double dist = 0;        // cm from where the strike point was last frame
};

// Which touch is the hit, or -1: the first target that lies on this side of
// whatever stopped the sweep (and of `wall_before`, a wall another check
// found, plus the ball's radius). The first blocking thing that is not a
// target is a wall: its distance goes to *wall_at. Something that only
// overlaps (a trigger volume) is neither.
inline int PickTouch(const Touch* t, int n, double wall_before, double radius, double* wall_at) {
    for (int i = 0; i < n; ++i) {
        if (t[i].kind == TouchKind::kTarget) {
            if (t[i].dist <= wall_before + radius) return i;
            return -1;
        }
        if (t[i].kind == TouchKind::kOther && t[i].blocking) {
            if (wall_at && t[i].dist < *wall_at) *wall_at = t[i].dist;
            return -1;
        }
    }
    return -1;
}

// Each target is hit at most once per `cooldown` seconds, so one swing
// through a body is one hit, not one per frame.
class Cooldowns {
public:
    bool Ready(uintptr_t target, double now, double cooldown) const {
        for (const Entry& e : e_)
            if (e.target == target && now - e.at < cooldown && now >= e.at) return false;
        return true;
    }
    void Mark(uintptr_t target, double now) {
        Entry* slot = &e_[0];
        for (Entry& e : e_) {
            if (e.target == target) { slot = &e; break; }
            if (e.at < slot->at) slot = &e;
        }
        slot->target = target;
        slot->at = now;
    }
    void Clear() { for (Entry& e : e_) e = Entry{}; }

private:
    struct Entry { uintptr_t target = 0; double at = -1e9; };
    Entry e_[8];
};

// The butt-strike stance (owner 2026-11-08: "When the player goes in for a
// butt strike and their off hand is up high and their main hand has been
// brought near their head i want to set a custom pose for that, for the main
// hand"): the main hand near the eyes, the off hand higher than it, and the
// barrel well off where you look (so shouldering and aiming, even upwards,
// never count). Once in, it holds with some slack, and for hold_s after
// that, so the strike itself keeps the pose. Only a hand pose: it starts no
// action.
struct StanceTuning {
    double near_head_cm = 30.0;  // main hand at most this far from the eyes (real cm)
    double off_above_cm = 10.0;  // off hand at least this much higher than the main hand
    double off_view_deg = 50.0;  // barrel at least this far from the view direction
    double hold_s = 0.5;         // kept this long after leaving the stance
};
struct StanceInput {
    bool valid = false;          // both hands and the head tracked
    FVector main, off, eyes;     // real cm, one frame, Z up
    FVector view, barrel;        // directions (need not be unit length)
    double now = 0;              // s
};
class Stance {
public:
    // Slack once in: the stance is left only past these.
    static constexpr double kNearSlackCm = 10.0, kAboveSlackCm = 6.0, kViewSlackDeg = 15.0;
    bool Update(const StanceInput& in, const StanceTuning& t) {
        head_cm = above_cm = view_deg = 0;
        bool strict = false, loose = false;
        if (in.valid) {
            head_cm = Length(in.main - in.eyes);
            above_cm = in.off.Z - in.main.Z;
            const double lv = Length(in.view), lb = Length(in.barrel);
            const double c = lv > 1e-6 && lb > 1e-6
                                 ? (in.view.X * in.barrel.X + in.view.Y * in.barrel.Y + in.view.Z * in.barrel.Z) / (lv * lb)
                                 : 1.0;
            view_deg = std::acos(std::clamp(c, -1.0, 1.0)) * 57.29577951308232;
            const bool ok = std::isfinite(head_cm) && std::isfinite(above_cm) && std::isfinite(view_deg);
            strict = ok && head_cm <= t.near_head_cm && above_cm >= t.off_above_cm && view_deg >= t.off_view_deg;
            loose = ok && head_cm <= t.near_head_cm + kNearSlackCm && above_cm >= t.off_above_cm - kAboveSlackCm &&
                    view_deg >= t.off_view_deg - kViewSlackDeg;
        }
        if (!on_) {
            on_ = strict;
            left_at_ = -1;
        } else if (loose) {
            left_at_ = -1;
        } else {
            if (left_at_ < 0 || in.now < left_at_) left_at_ = in.now;
            if (!(in.now - left_at_ < t.hold_s)) {
                on_ = false;
                left_at_ = -1;
            }
        }
        return on_;
    }
    bool On() const { return on_; }
    void Reset() { on_ = false; left_at_ = -1; }
    // Last measured, for the log.
    double head_cm = 0, above_cm = 0, view_deg = 0;

private:
    bool on_ = false;
    double left_at_ = -1;
};

}  // namespace holevr::vr::gun_melee
