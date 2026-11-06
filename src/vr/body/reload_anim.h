#pragma once
// The game's own left-hand reload animation, read for the VR hand (owner
// 2026-11-06: "the game has a left re-load animation ... It starts when my
// mag snaps into place and plays out as i push the mag in or the animation
// reverses as i move my hand away").
//
// The game reloads the pistol with ALS_HG_Isosceles_Reload_17R and the AR
// with ALS_AR_CClamp_Reload_30R. Each marks "Magazine Insert Begin" (the
// magazine at the magwell) and "Magazine Insert End" (seated: the game then
// moves it from the hand into the gun). In between the game holds the
// magazine at the hand socket HandItemPoint_L by the magazine's own
// AttachPoint_ReloadHand.
//
// We play the animation, paused, on a hidden copy of the character mesh,
// step it through that stretch once (a few frames per sample, under a second
// in all), keep the left hand's finger joints (each relative to its parent)
// and the socket (relative to the hand), and remove the copy. Body IK then
// poses the VR hand from them by how far the magazine is in (0 at the
// magwell, 1 seated), so pulling the magazine back plays it backwards.

#include <cstdint>
#include <string>

#include "vr/body/ik_math.h"

namespace holevr::vr::reload_anim {

enum Clip : int { kPistol = 0, kRifle = 1, kClipCount = 2 };
constexpr int kSamples = 12;
constexpr int kFingers = 5;  // thumb, index, middle, ring, pinky (hand_pose order)

struct Pose {
    FQuat joint[kFingers][3];  // each finger joint relative to its parent (joint 0: to the hand)
};

struct ClipData {
    bool ready = false;
    bool failed = false;
    std::string name;               // the animation
    std::string why;                // why it failed
    double length = 0;              // s
    double t_begin = 0, t_end = 0;  // "Magazine Insert Begin" / "Magazine Insert End", s into the animation
    Pose pose[kSamples];            // evenly from t_begin to t_end
    bool have_socket = false;       // HandItemPoint_L relative to the hand bone, at t_begin
    FQuat socket_rot;
    FVector socket_pos;
    double max_turn_deg[kFingers] = {};  // how far each finger turns in the stretch (log)
};

namespace detail {
inline FQuat Slerp(const FQuat& a, const FQuat& b, double t) {
    return ik::Normalized(a * ik::Scale(ik::Normalized(a.Inverse() * b), t));
}
}  // namespace detail

// The pose at progress p (0 = insert begins, 1 = seated), between samples.
inline void At(const ClipData& c, double p, Pose* out) {
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    const double x = p * (kSamples - 1);
    int i = static_cast<int>(x);
    if (i >= kSamples - 1) i = kSamples - 2;
    const double t = x - i;
    for (int f = 0; f < kFingers; ++f)
        for (int j = 0; j < 3; ++j)
            out->joint[f][j] = detail::Slerp(c.pose[i].joint[f][j], c.pose[i + 1].joint[f][j], t);
}

// How each joint has turned since the insert began, in the joint's own frame:
// start^-1 * at(p). Put after a pose's own joint rotation it plays the
// animation's finger motion on top of that pose.
inline void Delta(const ClipData& c, double p, Pose* out) {
    Pose now;
    At(c, p, &now);
    for (int f = 0; f < kFingers; ++f)
        for (int j = 0; j < 3; ++j)
            out->joint[f][j] = ik::Normalized(c.pose[0].joint[f][j].Inverse() * now.joint[f][j]);
}

// ---- engine side (reload_anim.cpp) ----
// Bones of the VR body's left hand, by name (the same names on the game's
// character mesh, whose pose the VR body copies by name).
struct Bones {
    uint64_t hand = 0;                 // FName, 8 bytes
    uint64_t finger[kFingers][3] = {};  // hand_pose finger order, base to tip
};

// Once per game frame from body IK, on the game thread: finds the
// animations in `clips` (bit 1 << Clip), samples them one at a time on a
// hidden copy of `char_mesh` (the game's animated character mesh, owned by
// `player`), then removes the copy. Nothing to do once they are read or
// have failed.
void Tick(uintptr_t player, uintptr_t char_mesh, const Bones& bones, unsigned clips);
const ClipData& Get(Clip c);
// Counts up each time a clip is read, so cached hand poses built from it
// can be rebuilt.
uint64_t Generation();

}  // namespace holevr::vr::reload_anim
