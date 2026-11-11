#pragma once
// Pending collision work belongs to the original raw controller intention.
// A withdrawal/opening input supersedes it; compatible retries retain their
// original age instead of postponing the deadline on every blocked frame.
#include <algorithm>
#include <cmath>
#include "vr/body/ik_math.h"

namespace holevr::vr::body::hand_contact_staging {
struct Intent {
    FVector position;
    FQuat rotation;
    double grip=0, trigger=0;
    bool thumb_touch=false, trigger_touch=false;
};
inline bool Compatible(const Intent& queued,const Intent& current,double age_seconds) {
    using namespace ik;
    const auto valid=[](const Intent& i) {
        const auto& q=i.rotation;
        const double norm=q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W;
        return std::isfinite(Length(i.position)) && std::isfinite(norm) && norm>1e-12 &&
               std::isfinite(i.grip) && std::isfinite(i.trigger);
    };
    if(!valid(queued)||!valid(current)||!std::isfinite(age_seconds)||age_seconds<0||age_seconds>.15)
        return false;
    const double movement=Length(current.position-queued.position);
    if(!std::isfinite(movement)||movement>.75 ||
       std::fabs(current.grip-queued.grip)>.12 || std::fabs(current.trigger-queued.trigger)>.12 ||
       current.thumb_touch!=queued.thumb_touch || current.trigger_touch!=queued.trigger_touch) return false;
    const FQuat a=Normalized(queued.rotation),b=Normalized(current.rotation);
    const double cosine=std::clamp(std::fabs(a.X*b.X+a.Y*b.Y+a.Z*b.Z+a.W*b.W),0.0,1.0);
    return cosine>=std::cos(4*kDegToRad); // quaternion half-angle: eight-degree rotation
}
}
