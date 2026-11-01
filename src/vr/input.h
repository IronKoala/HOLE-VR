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

// Call before the VR system starts (registers the OpenXR listener).
void Init();

// Latest controller state, consumed by the game thread once per game frame:
// the pressed/released edges accumulate between calls so short taps are not lost.
Snapshot Consume();
// Latest state without taking the button edges (for the menu pointer, which
// runs on the render thread and keeps its own edge state).
Snapshot Peek();

// Vibration, queued from any thread and played on the next frame.
void Haptic(Hand hand, float amplitude, float seconds, float frequency_hz = 0.f);

}  // namespace holevr::vr::input
