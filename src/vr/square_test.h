#pragma once
// Finds what draws the white dots ("white squares") into one eye during runs.
//
// The owner's 2026-11-03 10:47 screenshot shows them as rectangles of lone
// dots, one pixel in every 8 x 4 on a fixed grid of the picture, tracing
// rows of text, in one eye only, mostly noticed in dark places. Two kinds of
// thing can do that here:
//  - flat UI the game draws on its window (the eyes are cut out of the
//    window after the game's UI is drawn on it, so anything flat lands in
//    exactly one eye), or
//  - a render pass of the game reading a picture it never finished drawing.
//
// When the square watch (square_watch.h) sees many more lone dots in one eye
// than the other, this first lists the game's flat UI (every widget on the
// window and every widget component drawn flat) in the log, then switches
// suspects off one at a time for about a second each: each flat widget
// (made see-through), then render features (console variables), comparing
// the dots before, during and after. A suspect that clears the dots twice is
// logged as FOUND and, when switching it off costs nothing you'd notice, is
// left off for the rest of the run. Every switch is put back otherwise.
// square_test=0 in hole-vr.ini turns it off.
namespace holevr::vr::square_test {

// After game::Init: follows the player's tick.
void Init();

}  // namespace holevr::vr::square_test
