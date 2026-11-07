#include "vr/interact/holster_visual.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {
void HolsterVisual::Update(const HolsterVisualInput& in) {
    if (!in.enabled) { Reset(); return; }
    const double dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0, 0.1) : 0.0;
    const double radius = std::isfinite(in.radius) ? std::clamp(in.radius, 1.0, 50.0) : 12.0;
    if (in.hood_allowed)
        near_ = in.tracked && std::isfinite(in.distance) && in.distance >= 0 &&
                in.distance <= radius + (near_ ? 3.0 : 0.0);
    // A draw can happen in the first hover frame. Never leave a half-closed
    // hood in the muzzle path; empty/returning/parked always forces fully open.
    if (!in.seated) open_ = 1.0;
    else if (in.hood_allowed) {
        if (near_) open_ = std::min(1.0, open_ + dt / (8.0 / 30.0));
        else open_ = std::max(0.0, open_ - dt / (8.0 / 30.0));
    }

    // Menus, tracking loss and another held item release the visual hand at
    // once. Moving away with a still-free hand eases back to its real pose.
    if (!in.seated || !in.tracked || !in.actions || !in.hand_free || !in.hood_allowed) blend_ = 0.0;
    else if (near_) blend_ = std::min(1.0, blend_ + dt / 0.10);
    else blend_ = std::max(0.0, blend_ - dt / 0.08);
}
}  // namespace holevr::vr::interact
