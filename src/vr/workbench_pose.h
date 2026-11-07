#pragma once
#include "vr/math.h"

namespace holevr::vr::workbench {
enum class ReleaseHand { kLeft, kRight };

// Left/right as seen from the bench's player-facing edge, looking inward.
// The release hand chooses direction, independently of controller twist,
// the previous carried pose, or the arbitrary sign of the table's long axis.
inline bool BenchFrame(const FVector& outward, ReleaseHand hand, FQuat* frame) {
    const double length=std::hypot(outward.X,outward.Y);
    if (!std::isfinite(length) || length<1e-6) return false;
    const double sign=hand==ReleaseHand::kRight ? 1.0 : -1.0;
    const FVector barrel{-outward.Y/length*sign,outward.X/length*sign,0};
    *frame=YawOnly(std::atan2(barrel.Y,barrel.X)*kRadToDeg);
    return true;
}
}  // namespace holevr::vr::workbench
