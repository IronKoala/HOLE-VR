#pragma once
// Motion controller input through OpenXR: one action set with hand poses,
// trigger, grip, thumbstick and face buttons, bound for Valve Index (which
// SteamVR also maps other controllers onto), Oculus Touch and the generic
// simple profile. Registers itself with the OpenXR session (vr/openxr.h).

#include <openxr/openxr.h>

namespace holevr::vr::input {

enum Hand : int { kLeft = 0, kRight = 1 };

struct Button {
    bool down = false;
    bool pressed = false;   // went down since the previous snapshot
    bool released = false;  // went up since the previous snapshot
};

struct HandInput {
    bool active = false;          // the controller is on and bound
    bool grip_valid = false;      // grip pose tracked this frame
    bool aim_valid = false;
    XrPosef grip{{0, 0, 0, 1}, {0, 0, 0}};  // tracking space, metres
    XrPosef aim{{0, 0, 0, 1}, {0, 0, 0}};
    XrVector3f linear_velocity{0, 0, 0};    // grip, m/s, tracking space
    float trigger = 0.f;          // 0..1
    bool trigger_touch = false;
    float grip_value = 0.f;       // 0..1 squeeze
    float stick_x = 0.f, stick_y = 0.f;
    Button stick_click, primary, secondary, menu;
    Button trigger_button;        // trigger past the fire threshold (with hysteresis)
    Button grip_button;           // squeeze past the grab threshold (with hysteresis)
};

struct Snapshot {
    bool valid = false;
    XrTime time = 0;
    HandInput hand[2];
};

// Calibration-only, actively tracked poses, all located at the SAME base
// display time in the runtime's reference space (metres). No game prediction
// or last-known fallback. Rendering/gameplay validity is deliberately separate.
struct CalibrationTracking {
    bool valid = false;       // actively tracked head AND both hands
    bool head_valid = false;  // height remains measurable with resting/occluded hands
    XrTime time = 0;
    XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef grip[2] = {{{0, 0, 0, 1}, {0, 0, 0}}, {{0, 0, 0, 1}, {0, 0, 0}}};
};
// Extra head location work runs only while capture is enabled. Switching
// capture clears the published sample, including across disable/re-enable.
void SetCalibrationCapture(bool enabled);
CalibrationTracking CalibrationSample();

// Call before the VR system starts (registers the OpenXR listener).
void Init();

// Latest controller state, consumed by the game thread once per game frame:
// the pressed/released edges accumulate between calls so short taps are not lost.
// If a menu retired a new press after a release, gameplay sees down=false for
// that one Consume so an old hold ends. Peek still returns the real held level;
// the next gameplay tick requires a new press to start a new interaction.
// Poses are predicted for when the game's picture will be shown
// (xr::PredictAhead), to match the head pose the game draws with.
Snapshot Consume();
// Latest state without taking the button edges, poses at this headset frame's
// display time: for layers the headset draws itself (wrist display, laser).
Snapshot Peek();
// Active-menu read: atomically discard pending presses for both hands/all six
// buttons before invoking UI callbacks that may close the menu. Retains pending
// releases, raw button-down state, analog values, poses and LastConsumed.
Snapshot PeekForMenu();
// Like Peek, with the game's predicted-ahead poses: for things the game draws
// in its picture (the forearm menu).
Snapshot PeekGame();

// What the latest Consume returned (same poses, same edges), for checks
// that must see exactly the hand pose the game frame used.
Snapshot LastConsumed();

// Vibration, queued from any thread and played on the next frame.
void Haptic(Hand hand, float amplitude, float seconds, float frequency_hz = 0.f);
// Cancel queued pulses for this hand and stop its active runtime vibration.
// Does not discard the other hand's requests.
void StopHaptic(Hand hand);

}  // namespace holevr::vr::input
