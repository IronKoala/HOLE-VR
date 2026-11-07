#pragma once
// Each calibration state owns a complete instruction card. Callers must not
// append generic reason text or a second, conflicting movement instruction.
#include "vr/body/body_calibration.h"

namespace holevr::vr::calibration_guidance {
enum class Action { None, Paused, Prepare, Position, Capture, Complete, Failed };
enum class Illustration { None, Height, Forward, Out, Raising, Bent };
struct Guide {
    bool valid = false;
    Action action = Action::None;
    Illustration illustration = Illustration::None;
    char title[64] = {};
    char line[3][96] = {};
    int progress = 0;
};
// Covers all five stages and terminal/recovery states. Each instruction line
// fits 46 characters. No overhead or return-sweep instruction is emitted.
// pose_confirmed is the user's declaration. It is never inferred from an
// assumed body pose; capture tolerates natural movement. The caller renders
// the separate both-trigger hold / X-keep-arms / B-or-Y-cancel footer.
Guide ForStatus(const body_calibration::Status& status);
} // namespace holevr::vr::calibration_guidance
