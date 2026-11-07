#pragma once
// Gun melee (pistol whip, rifle butt and muzzle strikes): the parts of a held
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

// A pistol: the muzzle end of the slide, the middle of the slide (the side
// of the gun in a backhand whip) and the bottom of the grip (a hammer-fist
// strike with the magazine base plate). The grip point sits about two thirds
// of the way up the grip, so the base plate is ~7 cm under it and, with the
// grip's ~20 degree rake, ~2.5 cm behind it.
// A long gun: the butt (the measured stock pad when there is one), the
// muzzle and the middle of the handguard.
inline int StrikePoints(GunKind kind, double muzzle_dist, double bore_height, bool have_stock,
                        const FVector& stock, StrikePoint out[kMaxPoints]) {
    if (!std::isfinite(muzzle_dist) || muzzle_dist < 5.0) muzzle_dist = kind == GunKind::kPistol ? 18.0 : 45.0;
    if (!std::isfinite(bore_height) || std::fabs(bore_height) > 20.0) bore_height = 6.0;
    if (kind == GunKind::kPistol) {
        out[0] = {"muzzle", {muzzle_dist - 1.5, 0, bore_height}, 3.5};
        out[1] = {"slide", {muzzle_dist * 0.45, 0, bore_height}, 3.5};
        out[2] = {"grip base", {-2.5, 0, -7.0}, 3.5};
        return 3;
    }
    FVector butt = have_stock ? stock : FVector{-28.0, 0, bore_height - 3.0};
    if (!std::isfinite(butt.X) || butt.X > -10.0 || butt.X < -60.0) butt = FVector{-28.0, 0, bore_height - 3.0};
    out[0] = {"butt", butt, 5.0};
    out[1] = {"muzzle", {muzzle_dist, 0, bore_height}, 4.0};
    out[2] = {"handguard", {muzzle_dist * 0.5, 0, bore_height}, 4.0};
    return 3;
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

}  // namespace holevr::vr::gun_melee
