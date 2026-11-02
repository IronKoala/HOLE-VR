#pragma once
// Turns the engine's built-in "emulated stereo" device into a real headset
// device: the game is started with -emulatestereo, which makes UE create
// FFakeStereoRenderingDevice and render both eyes side by side into one
// texture. We swap that device's virtual functions for our own so the eyes
// use the headset's pose, field of view and resolution, and grab the eye
// texture to send to OpenXR.

#include "vr/math.h"
#include "vr/openxr.h"

namespace holevr::vr::stereo {

// Called from DllMain, before the engine reads its command line.
void InstallCommandLineHook();

// Called from the init thread after engine discovery and xr::InitInstance.
bool Install();

// Re-zero the headset position/yaw against the game camera.
void Recenter();

// Maps a pose in OpenXR tracking space (hands, controllers) into the game
// world with exactly the transform the eyes used on the latest frame
// (recentering, world scale, game camera position and yaw). Thread-safe;
// false until the first headset frame has been rendered.
bool TrackingToWorld(const XrPosef& tracking_pose, FVector* world_location, FQuat* world_rotation);

// Runs `fn` on the game thread once per rendered frame, right after both eye
// views are set up. Unlike the player's tick it keeps running while the game
// is paused and on the title screen, so menus can use it. One callback.
void SetGameFrameCallback(void (*fn)());

// Room-scale support. HeadOffsetWorld gives the horizontal offset (world
// axes, cm, Z = 0) of the player's head from the game camera on the latest
// frame, minus any shift already requested. After moving the pawn by some
// world delta, ShiftOriginWorld(delta) moves the tracking origin to match, so
// the view stays where it was. Horizontal only; both are thread-safe and
// return false until the first headset frame.
bool HeadOffsetWorld(FVector* offset_cm);
bool ShiftOriginWorld(const FVector& world_delta_cm);

}  // namespace holevr::vr::stereo
