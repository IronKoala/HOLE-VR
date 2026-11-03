#pragma once
// Finds bright blocks that are in one eye's picture but nowhere near the same
// place in the other eye's: what white squares and bars drawn into one eye
// look like. Pure math, unit tested in tests/test_square_watch.cpp.
//
// The two eyes see nearly the same scene, shifted sideways by how near things
// are (disparity: far away about 0, a hand at 30 cm about 150 pixels; things
// sit further right in the left eye than in the right eye). A bright block in
// one eye with nothing as bright within that shift in the other eye is odd.
// Highlights and the edges of near objects can do that too, a little; a burst
// of white squares does it a lot, in one eye.
#include <cstdint>
#include <vector>

namespace holevr::vr::square_watch {

// A horizontal strip of the picture, both eyes side by side (left eye on the
// left half), as brightness 0..1 per pixel, row by row.
struct Band {
    int width = 0;   // both eyes
    int height = 0;
    std::vector<float> luma;
};

struct Settings {
    int block = 8;        // block size, pixels
    int step = 4;         // search step in the other eye, pixels
    float bright = 0.8f;  // a block this bright (mean) is checked
    float margin = 0.3f;  // the other eye must have a block at most this much darker
    // Sideways shift searched, pixels, around the shift of the scene as a
    // whole (each eye's picture is off-centre, so far things are not at the
    // same x in both): from that much nearer than the scene to that much
    // further. The scene's shift is found within +-max_scene_shift. Nearer
    // than the scene goes up to a hand's shift below it, when hands fill the
    // strip and set the scene's shift.
    int min_shift = -160;
    int max_shift = 224;
    int max_scene_shift = 640;
    int max_up_down = 4;  // the eyes should line up vertically; a little slack
};

struct Odd {
    int count[2] = {0, 0};      // odd blocks in the left eye, the right eye
    int checked[2] = {0, 0};    // bright blocks checked
    int first_x[2] = {-1, -1};  // the first odd block (pixels, within that eye)
    int first_y[2] = {-1, -1};
    int scene_shift = 0;        // left eye x - right eye x of the scene, pixels
    // How many more odd blocks the worse eye has than the other, and which eye.
    int Score() const { return count[0] > count[1] ? count[0] - count[1] : count[1] - count[0]; }
    int Eye() const { return count[1] > count[0] ? 1 : 0; }
};

// Working memory FindOdd can keep between calls (a few MB for a strip of the
// picture), so a caller checking every frame doesn't allocate every frame.
struct Scratch {
    std::vector<double> sums;
};

Odd FindOdd(const Band& band, const Settings& s = Settings{}, Scratch* scratch = nullptr);

// The scene's sideways shift between the eyes (left eye x - right eye x), the
// one that makes the two halves of the band most alike.
int SceneShift(const Band& band, const Settings& s = Settings{});

// Brightness of one pixel of a read-back picture: R8G8B8A8, B8G8R8A8 or
// R10G10B10A2 as the bgra / ten_bit flags say.
float Luma(uint32_t px, bool bgra, bool ten_bit);

}  // namespace holevr::vr::square_watch
