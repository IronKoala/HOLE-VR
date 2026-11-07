#include "vr/interact/offhand_snap.h"
#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

void OffhandSnap::Update(const OffhandSnapInput& in) {
    const double dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0, 0.1) : 0.0;
    if (!in.allowed || !std::isfinite(in.hands_apart) || in.hands_apart < 0) {
        // A menu, tracking loss or a held item lets go at once.
        near_ = false;
        blend_ = 0.0;
        return;
    }
    const double on = std::isfinite(in.snap_cm) ? std::clamp(in.snap_cm, 1.0, 40.0) : 10.0;
    const double off = std::isfinite(in.release_cm) ? std::clamp(in.release_cm, on, 60.0) : on + 4.0;
    near_ = in.hands_apart <= (near_ ? off : on);
    const double chest_on = std::isfinite(in.chest_cm) ? std::clamp(in.chest_cm, 0.0, 150.0) : 35.0;
    const double chest_off =
        std::isfinite(in.chest_release_cm) ? std::clamp(in.chest_release_cm, chest_on, 200.0) : chest_on + 5.0;
    chest_ = std::isfinite(in.gun_to_chest) && in.gun_to_chest >= 0 &&
             in.gun_to_chest <= (chest_ ? chest_off : chest_on);
    if (near_) blend_ = std::min(1.0, blend_ + dt / 0.10);
    else blend_ = std::max(0.0, blend_ - dt / 0.08);
}

}  // namespace holevr::vr::interact
