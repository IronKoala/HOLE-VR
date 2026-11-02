#pragma once
// Body holsters: spots on the VR body that each hold one item, and the draw /
// put-away logic for them. Pure logic (no game calls), unit tested in
// tests/test_holster.cpp; vr/controls places the items and reads the hands.
//
// A spot is an offset in the body's frame (X forward, Y right, Z up, turned
// with the body's yaw only, so leaning does not tip it) from an anchor on the
// VR body: the belt (pelvis bone) or the chest (top spine bone). Offsets come
// from hole-vr.ini and can be moved in the headset, which saves them to
// hole-vr/holsters.ini.
//
// Slots now: right hip = pistol, left hip = pistol magazines (the magazine
// pouch). Planned for when the rifle is unlocked: a rifle slot (chest/back)
// and a rifle magazine slot; HolsterId and DefaultSpot already have room.

#include "vr/math.h"

namespace holevr::vr::interact {

enum class HolsterId : int { kPistol, kMag, kRifle, kRifleMag, kCount };
enum class HolsterAnchor : int { kBelt, kChest };

struct HolsterSpot {
    bool enabled = true;
    HolsterAnchor anchor = HolsterAnchor::kBelt;
    FVector offset;        // cm from the anchor, body frame (X forward, Y right, Z up)
    double radius = 12.0;  // grab / put-away zone around the spot, cm
    int hand = 1;          // 0 left, 1 right: the hand that takes from it
};

// Built-in spots, before hole-vr.ini and calibration.
HolsterSpot DefaultSpot(HolsterId id);
// "pistol", "mag", "rifle", "rifle_mag": ini keys are holster_<name>_*.
const char* HolsterName(HolsterId id);

// World position of a spot from its anchor's world location and the body's
// rotation (yaw only).
FVector SpotWorld(const HolsterSpot& s, const FVector& anchor, const FQuat& body_rot);
// The offset that puts a spot at `world` (calibration: "the holster goes
// where the hand is now").
FVector SpotOffset(const FVector& world, const FVector& anchor, const FQuat& body_rot);

// Gun frame (X barrel, Y right, Z up) of a pistol sitting in a strong-side
// hip holster: muzzle down, top of the slide toward the front, the gun's
// right side facing out from the body. cant_deg tilts the grip forward (FBI
// cant; the muzzle swings back).
FQuat PistolHolsterFrame(const FQuat& body_rot, double cant_deg);
// A gun frame whose magwell (raked rake_deg forward of the gun's up axis)
// stands straight up, facing the body's front. A magazine placed the way it
// sits in that gun stands upright in its pouch with the rounds facing forward.
FQuat MagPouchGunFrame(const FQuat& body_rot, double rake_deg);

// Taking an item out of a holster and putting it back, for one hand.
//   Hold mode: squeeze the grip inside the zone to take it; it stays in the
//   hand while the grip is held. Let go inside the zone and it is put away;
//   let go anywhere else and it goes back to the holster by itself.
//   Toggle mode: squeeze in the zone to take it, squeeze in the zone again to
//   put it away; letting go of the grip does nothing.
enum class DrawMode { kHold, kToggle };
enum class HolsterState { kHolstered, kInHand, kReturning };
enum class HolsterEvent { kNone, kHoverEnter, kDrawn, kStowed, kReturnStarted, kReturned };

struct DrawInput {
    FVector hand;               // the hand's grab point (controller grip), world cm
    FVector spot;               // the holster spot, world cm
    bool hand_valid = true;     // controller tracked
    bool spot_valid = true;     // holster position known
    bool grip_down = false;
    bool grip_pressed = false;  // went down this frame
    // Nothing changes while frozen (the grip editor borrows the controller,
    // or a menu is open): what is in the hand stays there.
    bool freeze = false;
    double dt = 0.0;
};

class HolsterDraw {
public:
    // radius: grab/put-away zone (cm). return_s: how long a let-go item takes
    // to fly back. settle_s: how long it takes to slide into place when put
    // away in the zone.
    void Configure(double radius, DrawMode mode, double return_s = 0.2, double settle_s = 0.08);
    HolsterEvent Update(const DrawInput& in);

    HolsterState State() const { return state_; }
    bool InHand() const { return state_ == HolsterState::kInHand; }
    // 0 at the moment it left the hand .. 1 sitting in the holster.
    double Blend() const { return blend_; }
    // Hand inside the zone while the item is in the holster.
    bool Hovering() const { return hover_; }
    // Distance from the hand to the spot at the last draw / put-away / let-go.
    double LastDistance() const { return last_dist_; }
    void Reset(HolsterState s = HolsterState::kHolstered);

private:
    double radius_ = 12.0;
    DrawMode mode_ = DrawMode::kHold;
    double return_s_ = 0.2;
    double settle_s_ = 0.08;
    double blend_speed_ = 0.0;   // blend per second while blending in
    HolsterState state_ = HolsterState::kHolstered;
    double blend_ = 1.0;
    bool hover_ = false;
    double last_dist_ = 0.0;
};

// Hand-in-zone tracker with a little hysteresis, for the "you're at the
// holster" haptic tick. Returns true on the frame the hand enters.
class HolsterZone {
public:
    bool Update(double dist, double radius, bool active);
    bool Inside() const { return inside_; }

private:
    bool inside_ = false;
};

}  // namespace holevr::vr::interact
