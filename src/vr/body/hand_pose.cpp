#include "vr/body/hand_pose.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::hand_pose {

namespace {
float Clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
}  // namespace

Curl Target(hands::GrabKind grab, const hands::FingerInput& in) {
    using hands::GrabKind;
    Curl c;
    const float trigger = Clamp01(in.trigger);
    const float grip = Clamp01(in.grip);
    switch (grab) {
        case GrabKind::kGunGrip:
            c.finger[kThumb] = 0.65f;
            // Trigger discipline: off the trigger the index lies straight
            // along the frame; touching it puts the pad on the trigger.
            c.finger[kIndex] = in.trigger_touch || trigger > 0.05f ? 0.38f + 0.27f * trigger : 0.05f;
            c.finger[kMiddle] = 0.82f;
            c.finger[kRing] = 0.85f;
            c.finger[kPinky] = 0.88f;
            break;
        case GrabKind::kSlideGrab:
            c.finger[kThumb] = 0.6f;
            c.finger[kIndex] = 0.55f;
            c.finger[kMiddle] = 0.58f;
            c.finger[kRing] = 0.6f;
            c.finger[kPinky] = 0.62f;
            break;
        case GrabKind::kMagHeld:
        case GrabKind::kMagInsert:
            c.finger[kThumb] = 0.45f;
            c.finger[kIndex] = 0.22f;
            c.finger[kMiddle] = 0.68f;
            c.finger[kRing] = 0.72f;
            c.finger[kPinky] = 0.75f;
            break;
        case GrabKind::kNone:
        case GrabKind::kHolster:
        default: {
            // Relaxed hand at rest, closing with the controller.
            const float rest = 0.12f;
            float index = in.trigger_touch ? 0.3f : rest;
            c.finger[kIndex] = std::max(index, rest + (0.95f - rest) * trigger);
            for (int f = kMiddle; f <= kPinky; ++f) c.finger[f] = rest + (0.95f - rest) * grip;
            c.finger[kThumb] = in.thumb_touch ? 0.55f : 0.15f + 0.4f * grip;
            break;
        }
    }
    return c;
}

void Smooth(Curl* current, const Curl& target, double dt, double speed) {
    double a = 1.0 - std::exp(-speed * std::max(0.0, dt));
    for (int f = 0; f < kFingerCount; ++f)
        current->finger[f] += static_cast<float>((target.finger[f] - current->finger[f]) * a);
}

JointLimits Limits(Finger f) {
    if (f == kThumb) return {35, 45, 55};
    return {75, 95, 65};
}

}  // namespace holevr::vr::hand_pose
