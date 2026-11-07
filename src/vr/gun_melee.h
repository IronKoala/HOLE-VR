#pragma once
// Gun melee: a held gun that is swung into something hits it. A pistol whips
// with its slide and muzzle or strikes with the bottom of its grip; a rifle
// (or any long gun) hits with its butt, muzzle or handguard. There is no
// button and no gesture to start it: the gun has to really touch the target
// while it moves at a hitting speed (gun_melee_min_speed). Slow contact is
// just a touch.
//
// A hit goes through the game's own hit event (the one its bullets, kick and
// enemy melee weapons use), so enemies, the hub's mannequins and shooting
// targets react the game's way. Damage is a share of the enemy's own max HP
// times the game's body-part multiplier; stun is a share of the kick's stun.
// Settings: gun_melee* in hole-vr.ini. Log lines start "gun melee:".
#include <cstdint>
#include <vector>

#include <openxr/openxr.h>

#include "vr/hands.h"
#include "vr/math.h"

namespace holevr::vr::gun_melee {

struct HeldGun {
    uintptr_t gun = 0;
    bool pistol = true;
    FQuat frame;                  // gun frame in the world (X barrel, Y right, Z up)
    FVector grip;                 // its origin: the firing grip point, world
    double muzzle_dist = 18.0;    // grip to muzzle along the bore, cm
    double bore_height = 6.0;     // bore above the grip point, cm
    bool have_stock = false;
    FVector stock;                // rifle butt pad, gun frame
    int hand = 1;                 // the controller it moves with (0 left, 1 right)
    hands::Pose hand_world;       // that controller this frame, world
    XrPosef hand_tracking{{0, 0, 0, 1}, {0, 0, 0}};  // and in tracking space (metres)
    int second_hand = -1;         // another hand on it (for the hit's buzz), -1 none
    bool kicking = false;         // a shot's kick is moving it: not a swing
};

struct Frame {
    uintptr_t player = 0;
    double clock = 0;             // controls clock, s
    uint64_t frame = 0;           // controls frame number (a gap forgets the last positions)
    int64_t xr_time = 0;          // XrTime of the poses, ns
    HeldGun guns[2];
    int count = 0;
    std::vector<uintptr_t> ignore;  // our own things the world check must not hit (guns, magazines, panels)
};

// Reads the settings and follows changes to them.
void Init();
// Once a game frame, while the player can fight (no menu, no grip editor).
void Update(const Frame& f);

}  // namespace holevr::vr::gun_melee
