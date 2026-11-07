#pragma once
// Presentation only. HolsterDraw remains the authority for drawing/stowing.
#include "vr/interact/holster.h"

namespace holevr::vr::interact {

inline bool PistolSeated(bool present, bool parked, HolsterState state, double settle) {
    return present && !parked && state == HolsterState::kHolstered && settle >= 1.0;
}

struct HolsterVisualInput {
    bool enabled = false;
    bool seated = false;
    bool tracked = false;
    bool actions = false;
    bool hand_free = false;
    bool hood_allowed = true; // another held item freezes seated hood/proximity
    double distance = 1e9;
    double radius = 12.0;
    double dt = 0.0;
};

class HolsterVisual {
public:
    void Update(const HolsterVisualInput& in);
    void Reset() { *this = HolsterVisual{}; }
    double HoodOpen() const { return open_; }
    double HandBlend() const { return blend_; }
    bool Near() const { return near_; }
private:
    bool near_ = false;
    double open_ = 1.0;   // no pistol: forced all the way down, including first frame
    double blend_ = 0.0;
};

}  // namespace holevr::vr::interact
