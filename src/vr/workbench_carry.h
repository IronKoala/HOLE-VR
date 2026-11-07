#pragma once
#include "vr/interact/rifle_carry.h"
#include "vr/interact/holster.h"

namespace holevr::vr::workbench {
// Only the last hand releasing the selected long gun may seat it. A right
// release with left support is a hand transfer, even over the vise.
inline bool MayPark(interact::CarryAction action, bool current) {
    return current && (action == interact::CarryAction::kRightPutsAway ||
                       action == interact::CarryAction::kLetGo);
}
inline bool MayPark(interact::HolsterEvent event) {
    return event==interact::HolsterEvent::kStowed || event==interact::HolsterEvent::kReturnStarted;
}
}  // namespace holevr::vr::workbench
