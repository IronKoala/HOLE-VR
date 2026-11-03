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
    // Charging handle (AR-15): the part the hand grabs is always forward
    // (travel 0), even when the bolt is locked back. The hand then has to
    // pull the handle back to the bolt before the bolt moves. Pistol slide:
    // false, the hand grabs the slide wherever it is.
    bool grab_at_closed = false;
    // Pistol: the slide can be grabbed anywhere along its length, not only at
    // the rear (front serrations, press checks). The zone runs this far forward
    // of grab_center (against the pull axis) as a capsule of radius
    // grab_radius_forward. 0 = rear sphere only.
    double grab_forward = 0.0;        // cm
    double grab_radius_forward = 6.0; // cm
    // Pistol: pinch grab from behind the slide (the hand is behind the rear
    // of the slide, on the bore line). Capsule this far back of grab_center
    // along the pull axis, radius grab_radius_back. 0 = none.
    double grab_back = 0.0;           // cm
    double grab_radius_back = 5.0;    // cm
    // Pistol, measured slide: grab anywhere within span_radius of the line
    // along the slide's top from span_front to span_rear, reaching at most
    // span_back_margin behind the rear end and span_front_margin past the
    // front end (flat caps: nothing further behind). When set, the rear
    // sphere and the forward/back capsules are not used.
    bool use_span = false;
    FVector span_rear, span_front;    // parent space, cm
    double span_radius = 8.5;
    double span_back_margin = 2.0;
    double span_front_margin = 2.0;
    // Pistol: pinch at the rear sight. Extra sphere, radius 0 = none.
    FVector sight_center;             // parent space, cm
    double sight_radius = 0.0;        // cm
    double sight_back = 0.0;          // capsule: runs this far back from the sight along the pull axis
};

enum class SlideEvent { kNone, kGrabbed, kReachedBack, kRacked, kPartialRelease, kSlipped };

// What one hold of the slide did, for the log (owner 2026-11-03: grabs from
// behind the slide caught but let go at 0-20% and the log couldn't say why).
// Distances in cm in the slide's parent space; pull is measured from where
// the hand was when it grabbed, + = back (the pull direction).
struct SlideHoldStats {
    int frames = 0;            // held frames after the grab frame
    double seconds = 0.0;
    double max_pull = 0.0;     // furthest the hand went back along the slide
    double min_pull = 0.0;     // furthest it went forward (<= 0)
    double last_pull = 0.0;    // where it was on the last held frame
    double max_drift = 0.0;    // furthest it moved away from the line it grabbed on
    double start_off_axis = 0.0;  // how far the grab was from the slide's line
    bool slipped = false;      // let go because the hand left the slide
};

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
    // Where the grabbed part itself is (0..1): the same as Travel() for a
    // slide; for a charging handle it can be ahead of a locked-back bolt.
    double HandleTravel() const { return held_ ? handle_ : 0.0; }
    bool InZone(const FVector& hand) const;
    // The current (or last) hold, reset on each grab.
    const SlideHoldStats& Stats() const { return stats_; }
    // A grab that the caller has already found inside a zone of its own
    // (e.g. the visible fingers at the rear sight) counts even when `hand`
    // is outside InZone. Applies to the next press only.
    void AcceptNextPress(bool accept) { accept_next_ = accept; }

private:
    SlideConfig cfg_;
    bool held_ = false;
    bool was_grab_down_ = false;
    bool reached_back_ = false;
    double start_ = 0.0;   // hand position along the axis when grabbed, minus travel then
    double travel_ = 0.0;
    double rest_ = 0.0;
    double peak_ = 0.0;
    double handle_ = 0.0;
    FVector grab_perp_;     // hand's offset from the slide's line when it grabbed
    double grab_along_ = 0.0;  // hand's position along the slide when it grabbed
    bool accept_next_ = false;
    SlideHoldStats stats_;
};

double Dot(const FVector& a, const FVector& b);
double Length(const FVector& a);

}  // namespace holevr::vr::interact
