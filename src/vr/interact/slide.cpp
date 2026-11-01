#include "vr/interact/slide.h"

#include <algorithm>
#include <cmath>

namespace holevr::vr::interact {

double Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
double Length(const FVector& a) { return std::sqrt(Dot(a, a)); }

bool Slide::InZone(const FVector& hand) const {
    return Length(hand - cfg_.grab_center) <= cfg_.grab_radius;
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
            start_ = along - travel_ * cfg_.length;
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
    travel_ = std::clamp((along - start_) / cfg_.length, rest_, 1.0);
    peak_ = std::max(peak_, travel_);
    if (!reached_back_ && travel_ >= cfg_.full_threshold) {
        reached_back_ = true;
        return SlideEvent::kReachedBack;
    }
    return SlideEvent::kNone;
}

}  // namespace holevr::vr::interact
