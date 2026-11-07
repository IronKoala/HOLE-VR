#pragma once
// Picking between the two rear pinch records of a pistol slide (see
// PickRearPinch in body.cpp): 0 = the pinch posed from above the slide,
// 1 = the pinch posed from behind it.

namespace holevr::vr::body::slide_pose {

// saved[i]: record i is posed (saved by the owner). turn[i]: how far the
// drawn hand would turn from the controller onto record i, degrees. A pose
// that turns the hand more than max_turn is not used. With neither fitting:
// an unposed from-behind pinch (built at the hand's own angle, so it turns
// nothing), else the posed one that turns less.
inline int ChooseRearPinch(const bool saved[2], const double turn[2], double max_turn) {
    const bool fits0 = saved[0] && turn[0] <= max_turn;
    const bool fits1 = saved[1] && turn[1] <= max_turn;
    if (fits0 && fits1) return turn[0] <= turn[1] ? 0 : 1;
    if (fits0) return 0;
    if (fits1) return 1;
    if (!saved[1]) return 1;
    return !saved[0] || turn[1] <= turn[0] ? 1 : 0;
}

}  // namespace holevr::vr::body::slide_pose
