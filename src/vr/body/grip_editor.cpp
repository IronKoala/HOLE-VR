#include "vr/body/grip_editor.h"

namespace holevr::vr::grip_editor {

using namespace ik;
using grip_record::kTipCount;

int Pick(const FVector& pointer, const Scene& scene) {
    double best = kGrabRadius;
    int item = kNoItem;
    for (int i = 0; i < kJointItems; ++i) {
        double d = Length(scene.joint[i] - pointer);
        if (d < best) best = d, item = i;
    }
    // Palm and object are bigger targets: picked unless a joint is clearly nearer.
    if (scene.has_palm) {
        double d = Length(scene.palm - pointer) - 1.5;
        if (d < best) best = d, item = kHandItem;
    }
    if (scene.has_object) {
        double d = Length(scene.object - pointer) - 1.5;
        if (d < best) best = d, item = kObjectItem;
    }
    return item;
}

Output Editor::Update(const Input& in, const Scene& scene, grip_record::Record* rec, FQuat* hand_rot,
                      FVector* hand_pos) {
    Output out;
    // Y: hold 1 s toggles, a short tap saves.
    if (in.y_down) {
        if (!y_was_down_) {
            y_time_ = 0;
            y_long_fired_ = false;
        }
        y_time_ += in.dt;
        if (!y_long_fired_ && y_time_ >= 1.0) {
            y_long_fired_ = true;
            if (active_) {
                out.save = true;
                out.exited = true;
                Stop();
            } else {
                active_ = true;
                out.entered = true;
            }
            out.haptic = 0.6f;
        }
    } else if (y_was_down_ && !y_long_fired_ && y_time_ < 0.4 && active_) {
        out.save = true;
        out.haptic = 0.4f;
    }
    y_was_down_ = in.y_down;
    if (!active_) {
        grab_was_down_ = in.grab_down;
        return out;
    }
    if (in.stick_click_pressed) {
        out.revert = true;
        grabbed_ = kNoItem;
        out.haptic = 0.4f;
    }

    // Grab the nearest item.
    if (in.grab_down && !grab_was_down_ && !out.revert) {
        grabbed_ = Pick(in.pointer, scene);
        if (grabbed_ != kNoItem) {
            pointer0_ = in.pointer;
            pointer_rot0_ = in.pointer_rot;
            hand_rot0_ = *hand_rot;
            hand_pos0_ = *hand_pos;
            out.started = true;
            out.haptic = 0.3f;
        }
    }
    if (!in.grab_down) grabbed_ = kNoItem;
    grab_was_down_ = in.grab_down;

    (void)rec;  // joints and the object are turned by the caller (vr/body)
    if (grabbed_ == kHandItem) {
        // The hand rides rigidly on the editing controller.
        FQuat dq = Normalized(in.pointer_rot * pointer_rot0_.Inverse());
        *hand_rot = Normalized(dq * hand_rot0_);
        *hand_pos = in.pointer + dq.Rotate(hand_pos0_ - pointer0_);
        out.changed = true;
    }
    out.grabbed = grabbed_;
    out.hover = grabbed_ != kNoItem ? grabbed_ : Pick(in.pointer, scene);
    return out;
}

}  // namespace holevr::vr::grip_editor
