#include "vr/interact/slide.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Length(const FVector& a) { return std::sqrt(Dot(a, a)); }

bool Slide::InZone(const FVector& hand) const {
    if (Length(hand - cfg_.grab_center) <= cfg_.grab_radius) return true;
    if (cfg_.grab_forward <= 0.0) return false;
    // Forward along the slide: distance to the segment from grab_center to
    // grab_center - axis * grab_forward.
    const double fwd = -Dot(hand - cfg_.grab_center, cfg_.axis);
    const double t = std::clamp(fwd, 0.0, cfg_.grab_forward);
    const FVector on_seg = cfg_.grab_center - cfg_.axis * t;
    return Length(hand - on_seg) <= cfg_.grab_radius_forward;
}

SlideEvent Slide::Update(const FVector& hand, bool grab_down, double dt) {
    const bool pressed = grab_down && !was_grab_down_;
    was_grab_down_ = grab_down;
    const double along = Dot(hand - cfg_.grab_center, cfg_.axis);

    if (!held_) {
        // Spring forward when not held.
        if (travel_ > rest_) travel_ = std::max(rest_, travel_ - cfg_.return_speed * dt);
        else travel_ = rest_;
        if (pressed && InZone(hand)) {
            held_ = true;
            reached_back_ = false;
            peak_ = travel_;
            const double grabbed_at = cfg_.grab_at_closed ? 0.0 : travel_;
            start_ = along - grabbed_at * cfg_.length;
            handle_ = grabbed_at;
            return SlideEvent::kGrabbed;
        }
        return SlideEvent::kNone;
    }

    // Off-axis distance: how far the hand is from the line the slide runs on.
    FVector on_axis = cfg_.grab_center + cfg_.axis * along;
    double off_axis = Length(hand - on_axis);

    auto let_go = [&](SlideEvent partial_event) {
        held_ = false;
        if (reached_back_) {
            reached_back_ = false;
            return SlideEvent::kRacked;
        }
        return partial_event;
    };

    if (!grab_down) return let_go(SlideEvent::kPartialRelease);
    if (off_axis > cfg_.max_off_axis) return let_go(SlideEvent::kSlipped);

    // A locked-open slide cannot go further forward than the catch.
    handle_ = std::clamp((along - start_) / cfg_.length, 0.0, 1.0);
    travel_ = std::clamp((along - start_) / cfg_.length, rest_, 1.0);
    peak_ = std::max(peak_, travel_);
    if (!reached_back_ && travel_ >= cfg_.full_threshold) {
        reached_back_ = true;
        return SlideEvent::kReachedBack;
    }
    return SlideEvent::kNone;
}

}  // namespace holevr::vr::interact
