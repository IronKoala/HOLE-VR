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

}  // namespace holevr::vr::stereo
