#pragma once
// Layout of the monitor view (spectator view): which part of the game
// window's side-by-side eye picture to show, and where in the window.
// Pure math so it can be unit tested natively (tests/test_spectator.cpp).

#include <cstdint>

namespace holevr::vr::spectator {

enum class View { kBothEyes, kRightEye, kLeftEye };

// Parses the monitor_view ini value: "both", "right" or "left" (any case).
// Anything else gives `fallback`.
View ParseView(const char* text, View fallback);

struct PixelRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

struct Layout {
    bool valid = false;
    PixelRect crop;  // in the window's picture (both eyes side by side)
    PixelRect dest;  // in the window; the rest of the window is black
};

// window_w/h: the game window's picture (left eye in the left half, right eye
// in the right). eye: 0 = left, 1 = right. center_u/center_v: where straight
// ahead is inside that eye's half, 0..1 from its left/top edge (the eye
// pictures are off-centre: each eye sees further to its own side).
// aspect: the shape to show (16/9); the crop keeps as much of the eye picture
// as that shape allows and is moved toward straight ahead but never past the
// eye's edges. It is scaled to fill the window's height or width, centred.
Layout ComputeLayout(uint32_t window_w, uint32_t window_h, int eye, float center_u, float center_v,
                     float aspect);

// Where straight ahead falls in an eye picture drawn with these OpenXR field
// of view angles (radians; left and down are negative). 0.5 if degenerate.
void StraightAhead(float angle_left, float angle_right, float angle_up, float angle_down, float* u, float* v);

}  // namespace holevr::vr::spectator
