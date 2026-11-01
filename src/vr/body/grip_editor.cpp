#include "vr/body/grip_editor.h"

namespace holevr::vr::grip_editor {

using namespace ik;
using grip_record::kTipCount;

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
        double best = kGrabRadius;
        grabbed_ = kNoItem;
        auto consider = [&](int item, const FVector& p) {
            double d = Length(p - in.pointer);
            if (d < best) {
                best = d;
                grabbed_ = item;
            }
        };
        consider(grip_record::kThumb, scene.tip[grip_record::kThumb]);
        consider(scene.index_state, scene.tip[scene.index_state]);
        for (int t = grip_record::kMiddle; t < kTipCount; ++t) consider(t, scene.tip[t]);
        // The palm is a bigger target: pick it unless a fingertip is clearly nearer.
        double palm_d = Length(scene.palm - in.pointer) - 1.5;
        if (palm_d < best) grabbed_ = kHandItem;
        if (grabbed_ != kNoItem) {
            pointer0_ = in.pointer;
            pointer_rot0_ = in.pointer_rot;
            hand_rot0_ = *hand_rot;
            hand_pos0_ = *hand_pos;
            if (grabbed_ >= 0) item0_ = rec->tip[grabbed_];
            out.haptic = 0.3f;
        }
    }
    if (!in.grab_down) grabbed_ = kNoItem;
    grab_was_down_ = in.grab_down;

    if (grabbed_ >= 0) {
        // Fingertip target follows the pointer's movement (not its absolute
        // position, so it doesn't jump on grab).
        rec->tip[grabbed_] = item0_ + (in.pointer - pointer0_);
        out.changed = true;
    } else if (grabbed_ == kHandItem) {
        // The hand rides rigidly on the editing controller.
        FQuat dq = Normalized(in.pointer_rot * pointer_rot0_.Inverse());
        *hand_rot = Normalized(dq * hand_rot0_);
        *hand_pos = in.pointer + dq.Rotate(hand_pos0_ - pointer0_);
        out.changed = true;
    }
    out.grabbed = grabbed_;
    return out;
}

}  // namespace holevr::vr::grip_editor
