#pragma once
// Hand-driven slide / bolt / charging handle. Pure logic: give it the off
// hand's position in the slide's parent space each frame and it says where
// the slide is and when the game should cycle the action.
//
//   - Grab inside the grab zone locks the hand to the slide axis.
//   - Travel = hand movement along the axis / full travel length.
//   - Reaching the back (>= full_threshold) once in a stroke = the round in
//     the chamber is extracted ("pull" half of a rack).
//   - Releasing after reaching the back = the slide runs forward and feeds a
//     round ("release" half). Releasing earlier = partial rack / chamber
//     check: the slide returns and nothing is ejected or fed.
//   - The hand slipping off the axis lets go automatically.

#include "vr/math.h"

namespace holevr::vr::interact {

struct SlideConfig {
    FVector grab_center;              // parent space, cm (e.g. the rear of the slide)
    double grab_radius = 7.0;         // cm
    FVector axis{-1, 0, 0};           // direction the slide moves when pulled
    double length = 4.7;              // full travel, cm
    double full_threshold = 0.9;      // fraction of travel that counts as racked
    double max_off_axis = 9.0;        // cm away from the axis before the hand slips
    double return_speed = 25.0;       // travel per second when springing forward
};

enum class SlideEvent { kNone, kGrabbed, kReachedBack, kRacked, kPartialRelease, kSlipped };

class Slide {
public:
    explicit Slide(const SlideConfig& c = SlideConfig()) : cfg_(c) {}
    void Configure(const SlideConfig& c) { cfg_ = c; }
    // Where the slide rests when not held: 0 normally, or the slide-catch
    // position while the gun is locked open.
    void SetRest(double rest) { rest_ = rest; }

    // hand: off-hand grip position in the slide's parent space (cm).
    // grab_down: grab button held this frame. dt: seconds.
    SlideEvent Update(const FVector& hand, bool grab_down, double dt);

    bool Held() const { return held_; }
    double Travel() const { return travel_; }   // 0 closed .. 1 fully back
    double Peak() const { return peak_; }       // furthest travel this stroke
    bool InZone(const FVector& hand) const;

private:
    SlideConfig cfg_;
    bool held_ = false;
    bool was_grab_down_ = false;
    bool reached_back_ = false;
    double start_ = 0.0;   // hand position along the axis when grabbed, minus travel then
    double travel_ = 0.0;
    double rest_ = 0.0;
    double peak_ = 0.0;
};

double Dot(const FVector& a, const FVector& b);
double Length(const FVector& a);

}  // namespace holevr::vr::interact
