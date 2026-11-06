#include "vr/body/calibration_guidance.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace holevr::vr::calibration_guidance {
namespace {
using body_calibration::Phase;
using body_calibration::Reason;
using body_calibration::HeightPosture;
constexpr const char* kComfort = "Keep both triggers held; small moves are OK.";

void Card(Guide& out, Action action, Illustration illustration, const char* title,
          const char* first, const char* second, const char* third) {
    out.valid = true;
    out.action = action;
    out.illustration = illustration;
    std::snprintf(out.title, sizeof(out.title), "%s", title);
    std::snprintf(out.line[0], sizeof(out.line[0]), "%s", first);
    std::snprintf(out.line[1], sizeof(out.line[1]), "%s", second);
    std::snprintf(out.line[2], sizeof(out.line[2]), "%s", third);
}
bool NeedsPause(Reason reason) {
    switch(reason) {
        case Reason::InvalidSample: case Reason::TimeWentBackwards:
        case Reason::OriginChanged: case Reason::HeadMoved:
        case Reason::TimedOut: case Reason::InvalidProfile:
        case Reason::HeightModeChanged: case Reason::StageTimedOut:
            return true;
        default: return false;
    }
}
} // namespace

Guide ForStatus(const body_calibration::Status& status) {
    Guide out;
    // Terminal results supersede stale per-frame reasons/readiness. Do not
    // advertise a measured elbow split when the core used prior/default data.
    if(status.phase == Phase::Complete) {
        if(!status.arms_measured)
            Card(out, Action::Complete, Illustration::None, "Setup complete",
                 status.height_measured ? "Your height baseline is ready." : "No new height baseline was measured.",
                 "Your arm settings are unchanged.", "You can return to playing.");
        else
            Card(out, Action::Complete, Illustration::None, "Setup complete",
                 "Your forward reach has been checked.",
                 status.reach_degraded ? "Some shoulder directions remain estimated." : "Your arm setup is ready.",
                 status.bend_degraded ? "Some arm proportions remain estimated." : "You can return to playing.");
        out.progress = 100;
        return out;
    }
    if(status.phase == Phase::Failed) {
        Card(out, Action::Failed, Illustration::None, "Setup stopped",
             status.reason == Reason::VerificationMismatch ? "Final reach differed from the earlier pose." :
             status.reason == Reason::BadArcFit ? "The arm poses did not give a clear fit." :
             status.reason == Reason::WristPoseMismatch ? "The wrist directions did not agree." :
             "This attempt could not be completed.", "Your saved fit is unchanged.",
             "Restart setup when you are ready.");
        return out;
    }
    if(status.phase != Phase::MeasureHeight && status.phase != Phase::ArmsForward &&
       status.phase != Phase::ArmsOut && status.phase != Phase::BendElbows &&
       status.phase != Phase::VerifyForward) return out;

    if(status.reason == Reason::TrackingLost) {
        Card(out, Action::Paused, Illustration::None, "Tracking paused",
             status.phase == Phase::MeasureHeight ? "Face forward in the tracking area." : "Bring both controllers into view.",
             "Wait for tracking to return.", "Your saved fit is unchanged.");
        return out;
    }
    if(NeedsPause(status.reason) || !std::isfinite(status.preparation_remaining_seconds)) {
        Card(out, Action::Paused, Illustration::None, "Setup paused",
             "Rest comfortably and look ahead.", "Wait for a fresh pose check.",
             "Your saved fit is unchanged.");
        return out;
    }

    const Action action = status.pose_confirmed ? Action::Capture : Action::Position;
    const char* reassurance = status.pose_confirmed ? kComfort : "Hold both index triggers for 2 seconds.";
    switch(status.phase) {
        case Phase::MeasureHeight:
            Card(out, action, Illustration::Height, "1/5 - Your comfortable posture",
                 status.height_posture == HeightPosture::Seated ? "Sit comfortably and look ahead." :
                 status.height_posture == HeightPosture::Standing ? "Stand comfortably and look ahead." :
                 "Use your usual seated or standing posture.",
                 status.reason == Reason::HeightUnavailable || status.reason == Reason::HeightUnspecified ?
                     "Your current height settings will be kept." : "Keep your head facing forward.",
                 reassurance);
            break;
        case Phase::ArmsForward:
        case Phase::VerifyForward:
            Card(out, action, Illustration::Forward,
                 status.phase == Phase::ArmsForward ? "2/5 - Arms forward" :
                 status.bend_degraded ? "5/5 - Check reach (estimated elbows)" : "5/5 - Check your forward reach",
                 "Straight arms forward, at a comfy height.",
                 "Palms inward; wrists in line with forearms.", reassurance);
            break;
        case Phase::ArmsOut:
            Card(out, action, Illustration::Out, "3/5 - Arms out to the sides",
                 "Open straight arms comfortably to the sides.",
                 "Palms down; wrists in line with forearms.", reassurance);
            break;
        case Phase::BendElbows:
            Card(out, action, status.pose_confirmed ? Illustration::Bent : Illustration::Raising,
                 "4/5 - Bend your elbows",
                 status.pose_confirmed ? "Stay near your comfortable bent pose." : "Bend elbows so your hands rise comfortably.",
                 "Keep elbows in place and wrists in line.",
                 status.pose_confirmed ? kComfort : "Hold both index triggers for 2 seconds.");
            break;
        default: break; // active phases were checked above
    }

    // Correct only the current requirement. Never append a second complete
    // instruction, ask for a return sweep, or blame ordinary sway.
    if(status.reason == Reason::LookForward) {
        Card(out, Action::Position, Illustration::None, "Look straight ahead",
             "Turn your head to face forward.",
             status.phase == Phase::MeasureHeight ? "Keep your usual comfortable posture." : "Keep your current arm position.",
             reassurance);
    } else if((status.reason == Reason::HandsTooLow || status.reason == Reason::ArmsNotLevel) &&
              (status.phase == Phase::ArmsForward || status.phase == Phase::ArmsOut ||
               status.phase == Phase::VerifyForward)) {
        // These are broad head-relative/observability checks, not a known
        // shoulder-height measurement. Preserve this stage's direction and
        // palm cue without asking the user to match an inferred joint height.
        std::snprintf(out.line[0], sizeof(out.line[0]), "%s", status.phase == Phase::ArmsOut
            ? "Reach to the sides at a comfortable height."
            : "Reach forward at a comfortable height.");
        out.action = Action::Position;
    } else if(status.reason == Reason::ArmDirectionMismatch) {
        // The stage already names the needed direction. Retain that complete
        // cue instead of replacing it with an unrelated height correction.
        out.action = Action::Position;
    } else if(status.reason == Reason::VerificationMismatch && status.phase == Phase::VerifyForward) {
        Card(out, Action::Position, Illustration::Forward, "5/5 - Match your earlier reach",
             "Return to your earlier straight-arm reach.",
             "Palms inward; wrists in line with forearms.", "Hold both index triggers for 2 seconds.");
    } else if(status.reason == Reason::BadArcFit && status.phase != Phase::MeasureHeight) {
        std::snprintf(out.title, sizeof(out.title), "%s", "Reach estimate is uncertain");
        std::snprintf(out.line[2], sizeof(out.line[2]), "%s", "Hold both triggers again. X: keep arms.");
        out.action = Action::Position;
    } else if(status.reason == Reason::WristPoseMismatch && status.phase != Phase::MeasureHeight) {
        // Retain the current pose/diagram; correct its wrist relationship
        // without importing the palm direction from a different stage.
        std::snprintf(out.title, sizeof(out.title), "%s", "Align wrists with your forearms");
        std::snprintf(out.line[2], sizeof(out.line[2]), "%s", "Hold both triggers again. X: keep arms.");
        out.action = Action::Position;
    }
    if(out.action == Action::Capture && std::isfinite(status.hold_fraction))
        out.progress = static_cast<int>(std::clamp(status.hold_fraction, 0.0, 1.0) * 10) * 10;
    return out;
}
} // namespace holevr::vr::calibration_guidance
