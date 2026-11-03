#pragma once
// How far the game camera moves between one frame's eye views and the next,
// predicted during the player's tick. Pure math, unit tested in
// tests/test_view_lead.cpp.
//
// The character's movement runs before the player's tick, but the game
// camera only follows it after the tick. Hands and held things placed in the
// world during the tick from the latest views' camera would trail the next
// views by one frame of the character's movement: a steady gap at a steady
// frame rate (the base), a jitter when frame times vary (a busy match).
#include <cstdint>

#include "vr/math.h"

namespace holevr::vr::view_lead {

// The parts of the character the camera may hang from, at one moment.
struct Anchors {
    uintptr_t who = 0;  // the player object, so a new level's player is never compared with the old
    bool camera = false;
    FVector camera_at;  // the player's camera component
    bool mesh = false;
    FVector mesh_at;  // the body mesh, with its turn
    FQuat mesh_rot;
    bool capsule = false;
    FVector capsule_at;  // the collision capsule (actor location)
    // The gun in the right hand, if any (for the log only).
    uintptr_t held = 0;
    FVector held_at;
    FQuat held_rot;
    // The right controller's grip pose this tick placed it from (tracking
    // space, UE axes, metres), for the log only.
    bool grip = false;
    FVector grip_m;
    FQuat grip_rot;
};

enum Kind : int { kCamera = 0, kMesh = 1, kCapsule = 2, kKinds = 3 };
extern const char* const kKindNames[kKinds];

// Each anchor's prediction of the camera's move since `then`, when the camera
// that drew the latest views was at `camera_then`. An anchor is not valid
// when it is missing at either moment, the player changed, or the move is
// longer than `max_cm` (a teleport, respawn or new level).
struct Leads {
    bool valid[kKinds] = {};
    FVector move[kKinds];
};
Leads Predict(const Anchors& then, const Anchors& now, const FVector& camera_then, double max_cm);

// Which prediction to use. choice: -1 the first valid of camera, body mesh,
// capsule; 0 none; 1, 2, 3 that one only. Returns the kind, or -1 for none.
int Pick(const Leads& l, int choice);

// How far off the hands would have been from the next views, per frame in
// which the camera moved (cm), for the log.
struct Stats {
    long long frames = 0;
    double none_sum = 0, none_max = 0;  // with no correction
    double used_sum = 0, used_max = 0;  // with the correction in use
    long long n[kKinds] = {};
    double sum[kKinds] = {};
    // actual: the camera's real move between the two views, minus the
    // room-scale steps (those are already taken off the tracking origin).
    void Add(const FVector& actual, const Leads& l, int used);

    // The view's turn between the two views (degrees). Hands are placed
    // before the tick's stick turn, so they trail it by this much.
    long long turn_frames = 0;
    double turn_sum = 0, turn_max = 0;
    void AddTurn(double degrees);

    // The corrected miss split along the view's facing (yaw_deg): forward,
    // sideways, up (cm, summed over the counted frames), and the worst few
    // frames with what each anchor predicted. Add() takes the yaw.
    double axis_sum[3] = {};
    struct Worst {
        double cm = 0;
        FVector miss;    // forward, side, up
        FVector actual;  // the camera's real move, world
        int used = -1;
        Leads leads;
    };
    static constexpr int kWorst = 3;
    Worst worst[kWorst];
    int worst_count = 0;
    void Add(const FVector& actual, const Leads& l, int used, double yaw_deg);
};

// How much the held gun shakes as seen from the eyes: the frame-to-frame
// change of its movement (second difference), in eye-yaw axes, so a steady
// or smoothly moving gun scores near 0 and a jittering one does not. Split
// by whether the character was moving. For the log.
struct GunShake {
    void Add(bool valid, const FVector& at, const FQuat& rot, bool moving);
    long long n[2] = {};         // [still, moving]
    double cm_sum[2] = {}, cm_max[2] = {};
    double deg_sum[2] = {}, deg_max[2] = {};
    // Moved by something else between the mod placing it and the next tick.
    long long moved_frames = 0, checked_frames = 0;
    double moved_max = 0;
    void AddMovedAfter(double cm);

    // The gun against the hand that holds it, as the eyes see both: `at` and
    // `rot` are the gun's pose in the controller's own axes. Held still in
    // the hand that is a fixed offset, however the hand or the character
    // moves, so its change from one picture to the next is shake the mod
    // adds (placement), not the player's hand or the tracking.
    void AddPlacement(bool valid, const FVector& at, const FQuat& rot, bool moving);
    long long place_n[2] = {};
    double place_cm_sum[2] = {}, place_cm_max[2] = {};
    double place_deg_sum[2] = {}, place_deg_max[2] = {};

    void ResetTotals();  // keeps the history

private:
    int have_ = 0;  // consecutive valid samples, up to 2
    FVector p1_, p2_;
    FQuat q1_, q2_;
    bool place_have_ = false;
    FVector place_p_;
    FQuat place_q_;
};

double AngleDeg(const FQuat& q);  // rotation angle of q, 0..180

double Length(const FVector& v);

}  // namespace holevr::vr::view_lead
