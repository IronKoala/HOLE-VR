#pragma once
// In-headset grip editor: tune a stored grip (grip_record.h) by eye.
//
// While a hand holds something with a grip record (the pistol in the right
// hand, a magazine or the slide in the left), the OTHER controller edits it:
//   - hold Y (left controller) for 1 s: start / save and stop editing;
//   - squeeze grip with the editing hand near a fingertip: drag that
//     fingertip's target, the finger re-solves live; near the palm: move and
//     turn the whole hand on the object;
//   - tap Y: save; click the left stick: undo back to the saved grip.
// The index finger edits whichever trigger state is showing (off the
// trigger, resting on it, or pulled), so pull the real trigger to edit the
// pulled pose.
//
// Engine-free state machine; body.cpp feeds it controller poses in the
// object's frame and applies the result. Unit tested in tests/test_grip.cpp.

#include "vr/body/grip_record.h"

namespace holevr::vr::grip_editor {

constexpr int kHandItem = -1;
constexpr int kNoItem = -2;

struct Input {
    double dt = 0;
    bool y_down = false;           // left Y / secondary button
    bool stick_click_pressed = false;
    bool grab_down = false;        // editing controller's grip
    FVector pointer;               // editing controller's grab point, object frame
    FQuat pointer_rot;             // editing controller's rotation, object frame
};

// Where things are right now, object frame.
struct Scene {
    FVector tip[grip_record::kTipCount];  // actual fingertips (all index entries = the index tip)
    FVector palm;                         // centre of the palm
    int index_state = grip_record::kIndexOff;  // which index target is showing
};

struct Output {
    bool entered = false, exited = false;
    bool save = false, revert = false;
    bool changed = false;      // record or hand moved this frame
    int grabbed = kNoItem;     // item being dragged
    float haptic = 0.f;        // pulse strength for the editing hand, 0 = none
};

class Editor {
public:
    bool Active() const { return active_; }
    // hand_rot/hand_pos: the hand bone in the object frame, moved when the
    // palm is dragged (the caller stores it back into the record).
    Output Update(const Input& in, const Scene& scene, grip_record::Record* rec, FQuat* hand_rot, FVector* hand_pos);
    void Stop() { active_ = false; grabbed_ = kNoItem; }

private:
    bool active_ = false;
    bool y_was_down_ = false;
    double y_time_ = 0;
    bool y_long_fired_ = false;
    bool grab_was_down_ = false;
    int grabbed_ = kNoItem;
    FVector pointer0_, item0_;
    FQuat pointer_rot0_, hand_rot0_;
    FVector hand_pos0_;
};

// Grab reach: items further than this from the pointer are not picked, cm.
constexpr double kGrabRadius = 6.0;

}  // namespace holevr::vr::grip_editor
