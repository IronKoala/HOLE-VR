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
};

double Length(const FVector& v);

}  // namespace holevr::vr::view_lead
