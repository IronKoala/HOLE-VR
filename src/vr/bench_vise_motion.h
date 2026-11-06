#pragma once
#include <algorithm>
#include <cmath>
#include "vr/math.h"

namespace holevr::vr::bench_vise::motion {
inline constexpr double kOpenGap = 8.0;
inline constexpr double kClosedGap = 0.6;
inline constexpr double kGripClearance = 0.2;
inline constexpr double kFixedJawFace = 3.78;
inline constexpr double kContactHeight = 13.275;
inline constexpr double kDuration = 0.45;
inline constexpr double kDefaultLoadedGap = 3.396;

inline double LoadedGap(double width) {
    if (!std::isfinite(width) || width <= 0) return kDefaultLoadedGap;
    return std::clamp(width + kGripClearance, kClosedGap, kOpenGap);
}
inline FVector GripCentre(double gap) { return {kFixedJawFace + gap*.5, 0, kContactHeight}; }
inline FVector HandSpot(const FVector& contact_world, const FQuat& gun_frame,
                        const FVector& contact_from_main, const FVector& hand_to_main) {
    return contact_world - gun_frame.Rotate(contact_from_main + hand_to_main);
}

// Stops exactly at each endpoint, reverses continuously on a quick pickup,
// and never moves the base. Caller supplies game time, so pausing freezes it.
class Jaw {
public:
    double Update(bool occupied, double loaded_gap, double dt) {
        const double goal = occupied && std::isfinite(loaded_gap)
            ? std::clamp(loaded_gap,kClosedGap,kOpenGap) : kOpenGap;
        if (std::fabs(goal-target_) > 1e-6) { start_=gap_;target_=goal;elapsed_=0; }
        if (std::isfinite(dt) && dt > 0) elapsed_ = std::min(kDuration,elapsed_+std::min(dt,.1));
        const double t=elapsed_/kDuration, eased=t*t*(3-2*t);
        gap_=start_+(target_-start_)*eased;
        return gap_;
    }
    double Gap() const { return gap_; }
    void Reset() { *this=Jaw{}; }
private:
    double gap_=kOpenGap, start_=kOpenGap, target_=kOpenGap, elapsed_=kDuration;
};
}  // namespace holevr::vr::bench_vise::motion
