#pragma once
// The off hand snapping onto the pistol in the other hand (owner 2026-11-07:
// "when my off hand controller is basically touching my main hand controller
// i want my off-hand to snap into position (the same way it snaps into
// position when it gets near the holster) i want 2 poses one for when the gun
// is near the chest and one for the rest of the time").
// Presentation only, like the hand snap near the holster: the off hand holds
// nothing and the controller is never moved. While it is on, the pistol
// counts as held in two hands (OffhandSnap::TwoHands).

namespace holevr::vr::interact {

struct OffhandSnapInput {
    // The pistol in the gun hand, the off hand free and tracked, no menu,
    // grip editor or calibration. false lets go at once.
    bool allowed = false;
    double hands_apart = 1e9;       // the two controllers' grip points, cm
    // The gun hand's controller to the chest, cm (1e9: no chest). The
    // controller, not the drawn gun: a kick or a wall push moves the gun.
    double gun_to_chest = 1e9;
    double snap_cm = 10.0;          // the off hand snaps on within this
    double release_cm = 14.0;       // and lets go beyond this
    double chest_cm = 35.0;         // the chest pose within this
    double chest_release_cm = 40.0; // back to the other pose beyond this
    double dt = 0.0;
};

class OffhandSnap {
public:
    void Update(const OffhandSnapInput& in);
    void Reset() { *this = OffhandSnap{}; }
    bool Near() const { return near_; }
    // Which of the two poses: the gun near the chest. While the hand is on
    // the gun it changes only once the other side has held for 0.15 s, so a
    // gun that jitters across the line never flips the pose every frame.
    bool Chest() const { return chest_; }
    // 0 the hand on its controller .. 1 fully on the gun (eases in 0.10 s,
    // out 0.08 s, like the holster's hand snap).
    double Blend() const { return blend_; }
    // The off hand is on the gun: the pistol is held in two hands.
    bool TwoHands() const { return near_; }

private:
    bool near_ = false;
    bool chest_ = false;
    double chest_pending_ = 0.0;  // the other pose wanted for this long, s
    double blend_ = 0.0;
};

}  // namespace holevr::vr::interact
