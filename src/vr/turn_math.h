#pragma once
// The turn stick's sideways push turned into degrees of turn for one frame,
// for each Turn style on the VR tab (turn_mode): 1 smooth, 0 snap, 2 off.
// Pure math, unit tested in tests/test_turn.cpp; controls.cpp Locomotion
// applies the result to the control rotation.
#include <cmath>

namespace holevr::vr::turn {

enum class Style { kSnap = 0, kSmooth = 1, kOff = 2 };

// turn_mode from the ini; anything unknown is smooth, the default (the VR
// tab shows such a value as Smooth too).
inline Style StyleFromMode(int turn_mode) {
    return turn_mode == 0 ? Style::kSnap : turn_mode == 2 ? Style::kOff : Style::kSmooth;
}

struct Settings {
    Style style = Style::kSmooth;
    double snap_degrees = 30.0;   // one flick, snap
    double smooth_speed = 90.0;   // deg/s at full stick, smooth
    double deadzone = 0.15;       // smooth: below this the stick is ignored
};

// Positive turns right. A snap fires once when the stick passes 0.7 and is
// armed again only after it comes back under 0.4 (`armed` carries that
// between frames). Off: never turns, and leaves a snap armed.
inline double Step(const Settings& s, double stick_x, double dt, bool* armed) {
    if (!std::isfinite(stick_x)) return 0.0;
    switch (s.style) {
        case Style::kOff:
            *armed = true;
            return 0.0;
        case Style::kSmooth:
            if (std::fabs(stick_x) > s.deadzone && dt > 0) return stick_x * s.smooth_speed * dt;
            return 0.0;
        case Style::kSnap:
            if (*armed && std::fabs(stick_x) > 0.7) {
                *armed = false;
                return stick_x > 0 ? s.snap_degrees : -s.snap_degrees;
            }
            if (std::fabs(stick_x) < 0.4) *armed = true;
            return 0.0;
    }
    return 0.0;
}

}  // namespace holevr::vr::turn
